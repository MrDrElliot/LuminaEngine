#include "RuntimePCH.h"
#include "World.h"
#include <cmath>
#include <utility>
#include "WorldManager.h"
#include "WorldContext.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/GeometryCollection/GeometryCollection.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Renderer/RHITexture.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Audio/AudioGlobals.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Core/Profiler/CPUProfiler.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "EASTL/sort.h"
#include "Animation/Pose.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Entity/EntityUtils.h"
#include "Entity/Components/CameraComponent.h"
#include "Entity/Components/SkeletalMeshComponent.h"
#include "Entity/Components/SocketAttachmentComponent.h"
#include "Entity/Components/DestructibleComponent.h"
#include "Entity/Components/DirtyComponent.h"
#include "Entity/Components/EditorComponent.h"
#include "Entity/Components/LifetimeComponent.h"
#include "Entity/Components/StaticMeshComponent.h"
#include "Entity/Components/DynamicMeshComponent.h"
#include "Entity/Components/FoliageComponent.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "Entity/Events/ImpulseEvent.h"
#include "entity/components/entitytags.h"
#include "Entity/Components/LineBatcherComponent.h"
#include "Entity/Components/TriangleBatcherComponent.h"
#include "Entity/Components/NameComponent.h"
#include "Entity/Components/PhysicsComponent.h"
#include "Entity/Components/PostProcessComponent.h"
#include "Entity/Components/TransformComponent.h"
#include "Entity/Components/WidgetComponent.h"
#include "Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"
#include "Entity/Components/SingletonEntityComponent.h"
#include "Entity/Systems/SystemSingletons.h"
#include "Entity/Systems/CameraSystem.h"
#include "entity/components/tagcomponent.h"
#include "Entity/Events/WorldEvents.h"
#include "Physics/Physics.h"
#include "Scene/RenderScene/RenderSceneFactory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "World/Entity/Components/CSharpScriptComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Net/NetRole.h"
#include "Networking/INetworkRuntime.h"
#include "Subsystems/WorldSettings.h"
#include "UI/RmlUiBridge.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/entity/systems/EntitySystem.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace ECS
    {
        // Engine-internal raw registry access (friended). Routes through CWorld's private accessor so the
        // registry stays off the public API; only whole-registry systems (serialization, net, meta) use this.
        FEntityRegistry& GetWorldRegistry(CWorld& World)
        {
            return World.GetEntityRegistry();
        }
    }

    namespace
    {
        template <typename TComponent> constexpr EPrimitiveSource PrimitiveSourceFor();
        template <> constexpr EPrimitiveSource PrimitiveSourceFor<SStaticMeshComponent>()   { return EPrimitiveSource::StaticMesh; }
        template <> constexpr EPrimitiveSource PrimitiveSourceFor<SDynamicMeshComponent>()  { return EPrimitiveSource::DynamicMesh; }
        template <> constexpr EPrimitiveSource PrimitiveSourceFor<SSkeletalMeshComponent>() { return EPrimitiveSource::SkeletalMesh; }

        // Clearing the component's stamp is what makes the resolve pre-pass revisit it; the tracker entry
        // is what makes the render scene's primitive for it re-read the component. Both are needed: the
        // first fixes the shared resolve, the second fixes this entity's cached render state.
        template <typename TComponent>
        void MarkMeshResolveDirty(FEntityRegistry& Registry, entt::entity Entity)
        {
            Registry.get<TComponent>(Entity).InvalidateRenderResolve();
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, PrimitiveSourceFor<TComponent>(),
                                                       EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
        }

        // Component (or its entity) going away. The resolve stamp is irrelevant now; only the render
        // scene needs telling, so its primitive is dropped instead of dangling.
        template <typename TComponent>
        void MarkMeshRemoved(FEntityRegistry& Registry, entt::entity Entity)
        {
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, PrimitiveSourceFor<TComponent>(),
                                                       EPrimitiveDirty::Membership);
        }

        void MarkFoliageResolveDirty(FEntityRegistry& Registry, entt::entity Entity)
        {
            for (SFoliageType& Type : Registry.get<SFoliageComponent>(Entity).Types)
            {
                Type.CachedEpoch = 0;
            }
            FMeshResolveCache::MarkPendingWork();
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage,
                                                       EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
        }

        void MarkFoliageRemoved(FEntityRegistry& Registry, entt::entity Entity)
        {
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Membership);
        }

        // Enable/disable is a membership change for every renderable component the entity might carry;
        // the sync pass drops the sources it doesn't actually have.
        void MarkRenderVisibilityDirty(FEntityRegistry& Registry, entt::entity Entity)
        {
            FRenderDirtyTracker::Ensure(Registry).MarkAllSources(Entity, EPrimitiveDirty::Visibility | EPrimitiveDirty::Membership);
        }
    }

    namespace
    {
        bool NetIsServerMode(ENetMode Mode)
        {
            return Mode == ENetMode::ListenServer || Mode == ENetMode::DedicatedServer;
        }

        // Shared FSystemFn for every C#-authored system: the FStageSlot's Self is the managed system's
        // GCHandle, so one shim forwards every managed tick to the right instance via the .NET host.
        void ManagedSystemUpdate(void* Self, const FSystemContext& Ctx) noexcept
        {
            DotNet::TickManagedSystem(Self, &Ctx);
        }
    }

    //~ World.Debug.* -- screen-space debug text + world debug shapes, forwarded to this world's draw
    // interface. Trailing args are optional; Dev/Debug only (the draws are no-ops in Shipping).
    void FWorldDebugInterface::DrawText(FStringView Text, TOptional<FVector4> Color)
    {
        if (World)
        {
            World->DrawDebugText(FString(Text), Color.value_or(FVector4(1.0f)));
        }
    }
    void FWorldDebugInterface::DrawLine(FVector3 Start, FVector3 End, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawLine(Start, End, Color, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }
    void FWorldDebugInterface::DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawBox(Center, HalfExtents, Rotation, Color, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }
    void FWorldDebugInterface::DrawSphere(FVector3 Center, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawSphere(Center, Radius, Color, 16, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }
    void FWorldDebugInterface::DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawCapsule(Start, End, Radius, Color, 16, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }
    void FWorldDebugInterface::DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawCone(Apex, Direction, AngleRadians, Length, Color, 16, 4, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }
    void FWorldDebugInterface::DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        if (World)
        {
            World->DrawArrow(Start, Direction, Length, Color, Thickness.value_or(1.0f), bDepthTest.value_or(true), Duration.value_or(0.0f));
        }
    }

    CWorld::CWorld()
        : SingletonEntity(entt::null)
        , SystemContext(this)
        , LineBatcherComponent(nullptr)
        , TriangleBatcherComponent(nullptr)
    {
        DebugInterface.World = this;
    }

    void CWorld::PaintRenderTarget(CTextureRenderTarget* Target, const FVector2& UV, float RadiusUV, const FVector4& Color, float Strength, float Hardness, CTexture* BrushMask)
    {
        if (Target == nullptr || !Target->GetTextureResource().NewTexture.IsValid())
        {
            return;
        }

        FTexturePaintOp Op;
        Op.Target       = Target->GetTextureResource().NewTexture.Texture;
        Op.TargetUAV    = RHI::Textures::StorageSlot(Target->GetTextureResource().NewTexture, 0);
        Op.TargetExtent = FUIntVector2(Target->GetWidth(), Target->GetHeight());
        Op.Mode         = FTexturePaintOp::EMode::Paint;
        Op.CenterUV     = UV;
        Op.RadiusUV     = RadiusUV;
        Op.Color        = Color;
        Op.Strength     = Strength;
        Op.Hardness     = Hardness;
        Op.BrushIndex   = (BrushMask != nullptr) ? BrushMask->GetResourceID() : -1;
        RenderTargetPaintQueue.enqueue(Move(Op));
    }

    void CWorld::ClearRenderTarget(CTextureRenderTarget* Target, const FVector4& Color)
    {
        if (Target == nullptr || !Target->GetTextureResource().NewTexture.IsValid())
        {
            return;
        }

        FTexturePaintOp Op;
        Op.Target       = Target->GetTextureResource().NewTexture.Texture;
        Op.TargetExtent = FUIntVector2(Target->GetWidth(), Target->GetHeight());
        Op.Mode         = FTexturePaintOp::EMode::Clear;
        Op.Color        = Color;
        RenderTargetPaintQueue.enqueue(Move(Op));
    }

    void CWorld::DrainRenderTargetPaints(TVector<FTexturePaintOp>& OutOps)
    {
        FTexturePaintOp Op;
        while (RenderTargetPaintQueue.try_dequeue(Op))
        {
            OutOps.push_back(Move(Op));
        }
    }

    void CWorld::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);

        if (Ar.IsReading())
        {
            RegistryPending.clear<>();
            ECS::Utils::SerializeRegistry(Ar, RegistryPending);
        }
        else
        {
            // A freshly-loaded asset keeps entities in RegistryPending until InitializeWorld swaps them
            // into EntityRegistry; DuplicateWorld serializes pre-init. Write from whichever holds the data.
            FEntityRegistry& Source = (!EntityRegistry.storage<entt::entity>().empty())
                ? EntityRegistry
                : RegistryPending;
            ECS::Utils::SerializeRegistry(Ar, Source);
        }
    }

    void CWorld::PreLoad()
    {
    }

    void CWorld::PostLoad()
    {
    }
    
    void CWorld::InitializeWorld(EWorldType InWorldType)
    {
        using namespace entt::literals;
        
        WorldType = InWorldType;
        
        CPrefab::CullOrphanedInstances(RegistryPending);
        
        if (!RegistryPending.storage<entt::entity>().empty())
        {
            EntityRegistry.swap(RegistryPending);
        }
        RegistryPending = {};

        CPrefab::RefreshAllInstancesInWorld(this);
        
        EntityRegistry.compact();
        
        // Which entities a client has no business holding is a netcode question, so it is reported
        // rather than decided here.
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->OnWorldEntitiesLoaded(this);
        }

        EntityRegistry.ctx().emplace<entt::dispatcher&>(SingletonDispatcher);
        
        auto WorldSettingsView = EntityRegistry.view<SDefaultWorldSettings>();
        for (auto Entity : WorldSettingsView)
        {
            if (!ALERT_IF_NOT(WorldSettingsView->size() == 1, "Multiple world settings were detected in the world! {}", WorldSettingsView->size()))
            {
                EntityRegistry.clear<SDefaultWorldSettings>();
                break;
            }
            
            SingletonEntity = Entity;
            break;
        }
        
        if (!EntityRegistry.valid(SingletonEntity))
        {
            SingletonEntity = EntityRegistry.create();
            EntityRegistry.emplace<SDefaultWorldSettings>(SingletonEntity);
        }
        
        LineBatcherComponent = &EntityRegistry.emplace<FLineBatcherComponent>(SingletonEntity);
        TriangleBatcherComponent = &EntityRegistry.emplace<FTriangleBatcherComponent>(SingletonEntity);
        EntityRegistry.emplace<FSingletonEntityTag>(SingletonEntity);
        EntityRegistry.emplace<FHideInSceneOutliner>(SingletonEntity);
        
        // Physics scene only for simulating worlds; Jolt reserves ~hundreds of MB up front.
        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            PhysicsScene = Physics::GetPhysicsContext()->CreatePhysicsScene(this);
        }
        // Emplaced even when null so ctx().get<>() consumers find the key (value is null in
        // non-simulating worlds and must be null-checked).
        EntityRegistry.ctx().emplace<Physics::IPhysicsScene*>(PhysicsScene.get());
        EntityRegistry.ctx().emplace<FSystemContext&>(SystemContext);
        EntityRegistry.ctx().emplace<CWorld*>(this);

        // Per-world subsystem singleton ticked by its system: STimerSystem advances FTimerManager
        // (FrameStart). Reached by ctx address.
        EntityRegistry.ctx().emplace<FTimerManager>();

        // System-produced singletons: SCameraSystem owns FCameraGlobalState (active camera + blend) and
        // writes FResolvedSceneView (read in Extract).
        EntityRegistry.ctx().emplace<FCameraGlobalState>();
        EntityRegistry.ctx().emplace<FResolvedSceneView>();

        CreateRenderer();
        UIContext = RmlUi::CreateWorldUI(this);

        // Seed the per-world disabled-system set from the saved world settings before registering systems,
        // so disabled systems are never constructed/started. Tolerant of stale names (ignored below).
        DisabledSystems.clear();
        for (const FName& Name : GetDefaultWorldSettings().DisabledSystems)
        {
            DisabledSystems.insert(Name);
        }
        PendingDisabledSystems = DisabledSystems;

        RegisterSystems();
        
        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            PhysicsScene->Simulate();
        }
        
        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            if (System.Startup)
            {
                System.Startup(System.Self, SystemContext);
            }
        });

        EntityRegistry.on_destroy   <FRelationshipComponent>()      .connect<&ThisClass::OnRelationshipComponentDestroyed>(this);
        EntityRegistry.on_construct <STransformComponent>()         .connect<&ThisClass::OnTransformComponentConstruct>(this);
        EntityRegistry.on_destroy   <SScriptComponent>()      .connect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityRegistry.on_destroy   <SWidgetComponent>()            .connect<&ThisClass::OnWidgetComponentDestroyed>(this);
        EntityRegistry.on_construct <SInputComponent>()             .connect<&ThisClass::OnInputComponentConstruct>(this);
        SystemContext.EventSink     <FSwitchActiveCameraEvent>()    .connect<&ThisClass::OnChangeCameraEvent>(this);

        // on_construct catches spawns/prefabs/loads; on_update catches registry.patch<T> edits;
        // on_destroy catches component removal and entity destruction, which is what keeps the render
        // scene's persistent primitive table from outliving what it was built from.
        EntityRegistry.on_construct <SStaticMeshComponent>()  .connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.on_update    <SStaticMeshComponent>()  .connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.on_destroy   <SStaticMeshComponent>()  .connect<&MarkMeshRemoved<SStaticMeshComponent>>();
        EntityRegistry.on_construct <SDynamicMeshComponent>() .connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.on_update    <SDynamicMeshComponent>() .connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.on_destroy   <SDynamicMeshComponent>() .connect<&MarkMeshRemoved<SDynamicMeshComponent>>();
        EntityRegistry.on_construct <SSkeletalMeshComponent>().connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.on_update    <SSkeletalMeshComponent>().connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.on_destroy   <SSkeletalMeshComponent>().connect<&MarkMeshRemoved<SSkeletalMeshComponent>>();
        EntityRegistry.on_construct <SFoliageComponent>()     .connect<&MarkFoliageResolveDirty>();
        EntityRegistry.on_update    <SFoliageComponent>()     .connect<&MarkFoliageResolveDirty>();
        EntityRegistry.on_destroy   <SFoliageComponent>()     .connect<&MarkFoliageRemoved>();
        EntityRegistry.on_construct <SDisabledTag>()          .connect<&MarkRenderVisibilityDirty>();
        EntityRegistry.on_destroy   <SDisabledTag>()          .connect<&MarkRenderVisibilityDirty>();

        // Components loaded before these hooks connected never saw on_construct.
        FMeshResolveCache::MarkPendingWork();
        // Same reason, for the primitive table: a full rescan is the only way to pick up what predates
        // the hooks (world load, level swap, editor world duplication).
        FRenderDirtyTracker::Ensure(EntityRegistry).RequestFullRescan();

        ECS::Utils::FTransformDirtyState* DirtyState = ECS::Utils::EnsureTransformDirtyState(EntityRegistry);
        auto TransformView = EntityRegistry.view<STransformComponent>();
        TransformView.each([&](entt::entity Entity, STransformComponent& TransformComponent)
        {
            TransformComponent.Registry = &EntityRegistry;
            TransformComponent.Entity = Entity;
            TransformComponent.DirtyState = DirtyState;
        });

        // Bind loaded input components to this world so their queries resolve to this world's viewport
        // (hooks connect after the load swap, so pre-existing components miss on_construct).
        auto InputView = EntityRegistry.view<SInputComponent>();
        InputView.each([&](SInputComponent& InputComponent)
        {
            InputComponent.World = this;
        });
        
        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            const auto AnyCameraView = EntityRegistry.view<SCameraComponent>();
            if (AnyCameraView.begin() == AnyCameraView.end())
            {
                LOG_WARN("CWorld::Initialize: world '{}' has no camera entity; spawning a fallback at (0, 2, 5) looking at origin. Add a camera entity for proper gameplay.", GetName());

                constexpr FVector3 FallbackPos(0.0f, 2.0f, 5.0f);

                const entt::entity Fallback = EntityRegistry.create();
                STransformComponent& Xf = EntityRegistry.emplace<STransformComponent>(Fallback);
                Xf.LocalTransform.SetLocation(FallbackPos);
                Xf.LocalTransform.SetRotation(Math::FindLookAtRotation(FVector3(0.0f), FallbackPos));

                SCameraComponent& Cam = EntityRegistry.emplace<SCameraComponent>(Fallback);
                Cam.bAutoActivate = true;
            }
        }

        auto CameraView = EntityRegistry.view<SCameraComponent>(entt::exclude<SDisabledTag>);
        CameraView.each([&](entt::entity Entity, const SCameraComponent& Camera)
        {
           if (Camera.bAutoActivate)
           {
               SingletonDispatcher.trigger<FSwitchActiveCameraEvent>(FSwitchActiveCameraEvent{Entity});
           }
        });

        if (WorldType == EWorldType::Simulation || WorldType == EWorldType::Game)
        {
            bPaused = false;
        }
        
        bInitializing = false;
    }
    
    void CWorld::TeardownWorld()
    {
        // No render phase / RHI / audio device in a headless process.
        if (!GIsHeadless)
        {
            RHI::WaitDeviceIdle();
        }

        RmlUi::DestroyWorldUI(this);
        UIContext.reset();

        if (GAudioContext != nullptr)
        {
            GAudioContext->StopAllSounds();
        }

        EntityRegistry.on_destroy<FRelationshipComponent>().disconnect<&ThisClass::OnRelationshipComponentDestroyed>(this);

        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            if (System.Teardown)
            {
                System.Teardown(System.Self, SystemContext);
            }
        });

        // Release this world's C# system instances (OnTeardown + GCHandle free).
        DestroyManagedSystems();

        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            PhysicsScene->StopSimulate();
        }

        EntityRegistry.ctx().get<FTimerManager>().Clear();

        RegistryPending.clear<>();
        EntityRegistry.clear<>();
        PhysicsScene.reset();
        DestroyRenderer();

        FCoreDelegates::PostWorldUnload.Broadcast();
    }
    
    static const char* StageName(EUpdateStage Stage)
    {
        switch (Stage)
        {
        case EUpdateStage::FrameStart:    return "FrameStart";
        case EUpdateStage::PrePhysics:    return "PrePhysics";
        case EUpdateStage::DuringPhysics: return "DuringPhysics";
        case EUpdateStage::PostPhysics:   return "PostPhysics";
        case EUpdateStage::FrameEnd:      return "FrameEnd";
        case EUpdateStage::Paused:        return "Paused";
        default:                          return "Unknown";
        }
    }

    void CWorld::Update(const FUpdateContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        const EUpdateStage Stage = Context.GetUpdateStage();

        FCPUProfiler::Get().PushWorldTarget(this);
        
        struct FPopGuard
        {
            ~FPopGuard()
            {
                FCPUProfiler::Get().PopTarget();
            }
        } PopGuard;

        CPU_PROFILE_SCOPE(StageName(Stage));

        // Reconcile any deferred system enable/disable before the gate/tick, so a toggle requested mid-frame
        // is applied here (between frames) and never inside a running system batch.
        ApplyPendingSystemChanges();

        // The script generation bumped, so the ManagedSystems hold GCHandles the managed
        // side already freed. Rebuild the system lists (drops stale slots, re-creates under the new
        // generation) before any tick so the shared shim never dereferences a freed handle. FrameStart
        // only, between frames, never inside a running batch.
        if (Stage == EUpdateStage::FrameStart && DotNet::IsInitialized()
            && DotNet::GetScriptGeneration() != ManagedSystemGeneration)
        {
            RegisterSystems();
        }

        if (Stage == EUpdateStage::FrameStart)
        {
            DeltaTime = Context.GetDeltaTime() * GetDefaultWorldSettings().DeltaTimeScale;
            TimeSinceCreation += DeltaTime;
        }

        if (bPaused && Stage != EUpdateStage::Paused || (!bPaused && Stage == EUpdateStage::Paused))
        {
            return;
        }

        SystemContext.DeltaTime     = DeltaTime;
        SystemContext.Time          = TimeSinceCreation;
        SystemContext.UpdateStage   = Stage;

        // Deferred timers run inside TickSystems now (STimerSystem, FrameStart/Highest), so they tick
        // before gameplay systems just as the old inline block did.
        {
            CPU_PROFILE_SCOPE("Systems");
            TickSystems(SystemContext);
        }
    }

    void CWorld::TickPhysics()
    {
        LUMINA_PROFILE_SCOPE();

        if (bPaused || PhysicsScene == nullptr)
        {
            return;
        }

        CPU_PROFILE_SCOPE_COLOR("Physics", FColor(0.20f, 0.75f, 0.90f));
        PhysicsScene->Update(DeltaTime);
    }

    void CWorld::DispatchPhysicsEvents()
    {
        if (PhysicsScene == nullptr)
        {
            return;
        }

        PhysicsScene->DispatchPendingEvents();
    }

    void CWorld::Extract()
    {
        LUMINA_PROFILE_SCOPE();
        DEBUG_ASSERT(Threading::IsMainThread());

        RmlUi::TickWorldUI(this);
        RmlUi::TickWorldWidgets(this);
        
        if (RenderScene == nullptr)
        {
            return;
        }
        
        const FResolvedSceneView& View = EntityRegistry.ctx().get<FResolvedSceneView>();

        if (View.bHasView)
        {
            RenderScene->SetActivePostProcessMaterials(View.PostProcessMaterials);
            RenderScene->Extract(View.ViewVolume, View.bHasPostProcess ? &View.PostProcess : nullptr);
            return;
        }

        RenderScene->SetActivePostProcessMaterials({});
        RenderScene->Extract(FViewVolume{}, nullptr);
    }

    entt::entity CWorld::ConstructEntity(FName Name, const FTransform& Transform)
    {
        DEBUG_ASSERT(Threading::IsMainThread(), "You may only construct entities on the main thread.");
        
        entt::entity NewEntity = GetEntityRegistry().create();
        
        if (Name == NAME_None)
        {
            Name = FName("Entity", entt::to_integral(NewEntity));
        }
     
        EntityRegistry.emplace<SNameComponent>(NewEntity, Name);
        EntityRegistry.emplace<STransformComponent>(NewEntity, Transform);

        return NewEntity;
    }

    entt::entity CWorld::SpawnProjectile(FVector3 Position, FVector3 Velocity, float Damage, float Lifetime, entt::entity Instigator)
    {
        FTransform SpawnTransform;
        SpawnTransform.SetLocation(Position);
        entt::entity Entity = ConstructEntity("Projectile", SpawnTransform);

        SProjectileComponent& Projectile = EntityRegistry.emplace<SProjectileComponent>(Entity);
        Projectile.Velocity = Velocity;
        Projectile.Damage = Damage;
        Projectile.Instigator = Instigator;

        // Reuse the engine lifetime system for auto-despawn.
        if (Lifetime > 0.0f)
        {
            EntityRegistry.emplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
        }
        return Entity;
    }

    bool CWorld::FractureEntity(entt::entity Entity, const FVector3& Origin, float Strength)
    {
        LUMINA_PROFILE_SCOPE();

        if (!EntityRegistry.valid(Entity))
        {
            return false;
        }

        SDestructibleComponent* Destructible = EntityRegistry.try_get<SDestructibleComponent>(Entity);
        if (Destructible == nullptr || Destructible->bFractured)
        {
            return false;
        }

        // Resolve the mesh to shatter: explicit fragment override, else the entity's own static mesh.
        SStaticMeshComponent* MeshComp = EntityRegistry.try_get<SStaticMeshComponent>(Entity);
        CStaticMesh* SourceMesh = Destructible->FragmentMesh.Get();
        if (SourceMesh == nullptr && MeshComp != nullptr)
        {
            SourceMesh = MeshComp->StaticMesh.Get();
        }

        if (SourceMesh == nullptr)
        {
            LOG_WARN("FractureEntity: entity {} has no mesh to fracture", entt::to_integral(Entity));
            return false;
        }

        FTransform OwnerTransform = EntityRegistry.get<STransformComponent>(Entity).GetWorldTransform();
        
        FVector3 InheritedVelocity(0.0f);
        if (PhysicsScene)
        {
            if (const SRigidBodyComponent* RB = EntityRegistry.try_get<SRigidBodyComponent>(Entity))
            {
                if (RB->BodyID != 0xFFFFFFFFu)
                {
                    OwnerTransform.SetLocation(PhysicsScene->GetBodyPosition(RB->BodyID));
                    OwnerTransform.SetRotation(PhysicsScene->GetBodyRotation(RB->BodyID));
                    InheritedVelocity       = PhysicsScene->GetLinearVelocity(RB->BodyID);
                }
            }
        }

        const FVector3 OwnerScale = OwnerTransform.GetScale();

        const float LaunchSpeed = Strength > 0.0f ? Strength : Destructible->ExplosionStrength;
        const float SpinSpeed   = Destructible->SpinStrength;

        // Deterministic per-fragment jitter (good for replays / lockstep): hash the index.
        auto Hash01 = [](uint32 V) -> float
        {
            V ^= V >> 16; V *= 0x7feb352dU; V ^= V >> 15; V *= 0x846ca68bU; V ^= V >> 16;
            return static_cast<float>(V) / static_cast<float>(0xFFFFFFFFU);
        };

        // Inherited momentum + an outward blast (radial from Origin) + random spin on a fresh body.
        auto LaunchBody = [&](uint32 BodyID, const FVector3& WorldCenter, uint32 Seed)
        {
            if (!PhysicsScene || BodyID == 0xFFFFFFFFu)
            {
                return;
            }
            FVector3 Direction = WorldCenter - Origin;
            const float Distance = Math::Length(Direction);
            Direction = Distance > 1e-4f
                ? Direction / Distance
                : Math::Normalize(FVector3(Hash01(Seed) - 0.5f, Hash01(Seed + 1) + 0.25f, Hash01(Seed + 2) - 0.5f));

            const float SpeedJitter = 0.7f + 0.6f * Hash01(Seed + 3);
            const FVector3 LaunchVelocity = InheritedVelocity
                + Direction * (LaunchSpeed * SpeedJitter)
                + FVector3(0.0f, LaunchSpeed * 0.2f, 0.0f);
            PhysicsScene->OnSetVelocityEvent(SSetVelocityEvent{ BodyID, LaunchVelocity });

            if (SpinSpeed > 0.0f)
            {
                const FVector3 Spin(Hash01(Seed + 4) - 0.5f, Hash01(Seed + 5) - 0.5f, Hash01(Seed + 6) - 0.5f);
                PhysicsScene->OnSetAngularVelocityEvent(SSetAngularVelocityEvent{ BodyID, Spin * (2.0f * SpinSpeed) });
            }
        };

        int32 Spawned = 0;

        // Source of pieces: an assigned collection if present, else a convex Voronoi fracture
        // generated on the fly from the mesh bounds (real chunks with zero authoring).
        const FFractureData* CollectionData = nullptr;
        if (CGeometryCollection* Collection = Destructible->Collection.Get())
        {
            if (Collection->GetNumPieces() > 0)
            {
                CollectionData = &Collection->GetFractureData();
            }
        }

        TVector<FFracturePiece> GeneratedPieces;
        if (CollectionData == nullptr)
        {
            FFractureSettings Settings;
            Settings.NumPieces = Destructible->FragmentCount;
            Settings.Seed      = entt::to_integral(Entity) * 2654435761U + 1u;
            Fracture::GenerateConvexFracture(SourceMesh, Settings, GeneratedPieces);
        }

        const TVector<FFracturePiece>& Pieces = CollectionData ? CollectionData->Pieces : GeneratedPieces;

        // Create all fragment bodies in one batch (AddBodiesPrepare/Finalize). BodyIDs valid only after
        // EndBodyBatch, so collect launch impulses and apply them once inserted.
        struct FPendingLaunch { entt::entity Fragment; FVector3 Center; uint32 Seed; };
        TVector<FPendingLaunch> PendingLaunches;
        PendingLaunches.reserve(Pieces.size());

        // Cap fragments at physics body headroom; overflowing Jolt's body/contact buffers trips a hard
        // assert, so clamp + warn instead. Raise World Settings > Physics > Max* for denser destruction.
        uint32 MaxFragments = 0xFFFFFFFFu;
        if (PhysicsScene)
        {
            const uint32 MaxBodies = PhysicsScene->GetMaxBodyCount();
            const uint32 Used      = Math::Min(PhysicsScene->GetBodyCount(), MaxBodies);
            const uint32 Headroom  = MaxBodies - Used;
            MaxFragments = Headroom > 16 ? Headroom - 16 : 0;

            const uint32 Desired = Pieces.empty()
                ? (uint32)Math::Clamp(Destructible->FragmentCount, 2, 512)
                : (uint32)Pieces.size();
            if (Desired > MaxFragments)
            {
                LOG_WARN("FractureEntity: clamped {} fragments to {} (physics body headroom {}/{}). Raise World Settings > Physics > MaxPhysicsBodies.",
                    Desired, MaxFragments, Used, MaxBodies);
            }
        }

        if (PhysicsScene)
        {
            PhysicsScene->BeginBodyBatch();
        }

        if (!Pieces.empty())
        {
            const TVector<TObjectPtr<CMaterialInterface>>& PieceMaterials =
                (CollectionData && !Destructible->Collection->Materials.empty())
                    ? Destructible->Collection->Materials
                    : SourceMesh->Materials;

            // Pre-baked collections cache piece meshes (built at load), so fracture does no per-piece
            // meshlet build / upload. The on-the-fly Voronoi path has no cache and builds each inline.
            const TVector<TObjectPtr<CStaticMesh>>* CachedMeshes =
                CollectionData ? &Destructible->Collection->GetPieceMeshes() : nullptr;

            for (size_t PieceIndex = 0; PieceIndex < Pieces.size() && (uint32)Spawned < MaxFragments; ++PieceIndex)
            {
                const FFracturePiece& Piece = Pieces[PieceIndex];

                CStaticMesh* PieceMesh = CachedMeshes
                    ? (PieceIndex < CachedMeshes->size() ? (*CachedMeshes)[PieceIndex].Get() : nullptr)
                    : Fracture::BuildPieceMesh(Piece, PieceMaterials, "GCPiece");
                if (PieceMesh == nullptr)
                {
                    continue;
                }

                // BuildPieceMesh recenters geometry to the piece centroid (natural pivot + CoM); place
                // the entity at the centroid's world position so pieces reconstruct the object at t=0.
                const FVector3 WorldCenter = OwnerTransform.GetLocation() + OwnerTransform.GetRotation() * (OwnerTransform.GetScale() * Piece.Center);
                FTransform PieceTransform;
                PieceTransform.SetLocation(WorldCenter);
                PieceTransform.SetRotation(OwnerTransform.GetRotation());
                PieceTransform.SetScale(OwnerTransform.GetScale());

                const entt::entity Fragment = ConstructEntity("Fragment", PieceTransform);
                EntityRegistry.emplace_or_replace<FNeedsTransformUpdate>(Fragment);

                EntityRegistry.emplace<SStaticMeshComponent>(Fragment).SetStaticMesh(PieceMesh);

                // The collider's on_construct builds the Jolt shape synchronously, so Mesh + bConvex must
                // be set before insertion -- otherwise the body uses default (non-convex) settings, forced Static.
                SMeshColliderComponent ColliderDesc;
                ColliderDesc.Mesh    = PieceMesh;
                ColliderDesc.bConvex = true;
                EntityRegistry.emplace<SMeshColliderComponent>(Fragment, std::move(ColliderDesc));

                EntityRegistry.emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.emplace<SFragmentComponent>(Fragment).Source   = entt::to_integral(Entity);
                
                PendingLaunches.push_back({ Fragment, WorldCenter, entt::to_integral(Fragment) + static_cast<uint32>(Spawned) });

                ++Spawned;
            }
        }
        else
        {
            // Fallback (degenerate fracture): subdivide the bounds into a grid of textured box chunks.
            const FAABB& LocalBounds = SourceMesh->GetAABB();
            const FVector3 LocalExtent = Math::Max(LocalBounds.GetSize(), FVector3(0.01f));
            const FVector3 LocalCenter = LocalBounds.GetCenter();
            const int32 Target = Math::Clamp(Destructible->FragmentCount, 2, 512);
            const int32 Dims   = Math::Max(1, static_cast<int32>(std::ceil(std::cbrt(static_cast<float>(Target)))));
            const FVector3 LocalCell = LocalExtent / static_cast<float>(Dims);
            const FVector3 FragScale = OwnerScale / static_cast<float>(Dims);
            const FVector3 ColliderHalf = LocalExtent * 0.5f;
            CStaticMesh* GridMesh = Destructible->FragmentMesh.Get() ? Destructible->FragmentMesh.Get() : SourceMesh;

            for (int32 zi = 0; zi < Dims && Spawned < Target && (uint32)Spawned < MaxFragments; ++zi)
            for (int32 yi = 0; yi < Dims && Spawned < Target && (uint32)Spawned < MaxFragments; ++yi)
            for (int32 xi = 0; xi < Dims && Spawned < Target && (uint32)Spawned < MaxFragments; ++xi)
            {
                const FVector3 CellLocalCenter = LocalBounds.Min + (FVector3(xi, yi, zi) + 0.5f) * LocalCell;
                const FVector3 CellWorldCenter = OwnerTransform.GetLocation() + OwnerTransform.GetRotation() * (OwnerScale * CellLocalCenter);

                FTransform FragmentTransform;
                FragmentTransform.SetLocation(CellWorldCenter - OwnerTransform.GetRotation() * (FragScale * LocalCenter));
                FragmentTransform.SetRotation(OwnerTransform.GetRotation());
                FragmentTransform.SetScale(FragScale);

                const entt::entity Fragment = ConstructEntity("Fragment", FragmentTransform);
                EntityRegistry.emplace_or_replace<FNeedsTransformUpdate>(Fragment);

                SStaticMeshComponent& FragmentMeshComp = EntityRegistry.emplace<SStaticMeshComponent>(Fragment);
                FragmentMeshComp.SetStaticMesh(GridMesh);
                if (MeshComp != nullptr)
                {
                    FragmentMeshComp.MaterialOverrides = MeshComp->MaterialOverrides;
                }

                // Box collider auto-emplaces a Dynamic rigid body, built synchronously from these
                // settings the instant the component is inserted, so set HalfExtent up front.
                SBoxColliderComponent BoxDesc;
                BoxDesc.HalfExtent = ColliderHalf;
                EntityRegistry.emplace<SBoxColliderComponent>(Fragment, std::move(BoxDesc));

                EntityRegistry.emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.emplace<SFragmentComponent>(Fragment).Source   = entt::to_integral(Entity);

                PendingLaunches.push_back({ Fragment, CellWorldCenter, entt::to_integral(Fragment) + static_cast<uint32>(Spawned) });

                ++Spawned;
            }
        }

        // Insert all the queued bodies at once, then apply the launch impulses now that BodyIDs exist.
        if (PhysicsScene)
        {
            PhysicsScene->EndBodyBatch();
        }
        for (const FPendingLaunch& Launch : PendingLaunches)
        {
            LaunchBody(EntityRegistry.get<SRigidBodyComponent>(Launch.Fragment).BodyID, Launch.Center, Launch.Seed);
        }

        Destructible->bFractured = true;

        // Retire the original: strip render + physics now so it vanishes this frame; the lifetime system
        // reaps it at FrameEnd -- safe even when called from the entity's own script callback.
        if (Destructible->bDestroyOriginal)
        {
            EntityRegistry.remove<SStaticMeshComponent>(Entity);
            EntityRegistry.remove<SRigidBodyComponent>(Entity);
            EntityRegistry.remove<SBoxColliderComponent>(Entity);
            EntityRegistry.remove<SSphereColliderComponent>(Entity);
            EntityRegistry.remove<SMeshColliderComponent>(Entity);
            EntityRegistry.emplace_or_replace<SLifetimeComponent>(Entity).Lifetime = 0.01f;
        }

        return Spawned > 0;
    }

    entt::entity CWorld::SpawnPrefab(const FAssetRef& Prefab)
    {
        return SpawnPrefabAt(Prefab, FTransform(), entt::null);
    }

    entt::entity CWorld::SpawnPrefabAt(const FAssetRef& Prefab, const FTransform& SpawnTransform, entt::entity Parent)
    {
        FStringView Path = Prefab.GetPath();
        FAssetData* AssetData = FAssetRegistry::Get().GetAssetByPath(Path);
        if (AssetData == nullptr)
        {
            LOG_WARN("SpawnPrefab: no asset found at path '{}'", Path);
            return entt::null;
        }

        CPrefab* PrefabObject = LoadObject<CPrefab>(AssetData->AssetGUID);
        if (PrefabObject == nullptr)
        {
            LOG_WARN("SpawnPrefab: asset '{}' is not a CPrefab", Path);
            return entt::null;
        }

        return PrefabObject->Instantiate(this, SpawnTransform, Parent);
    }

    void CWorld::SpawnPrefabAsync(const FName& Path, const TFunction<void(entt::entity)>& Callback)
    {
        AsyncLoadObject(Path, [this, Callback, Path](CObject* Object)
        {
            CPrefab* Prefab = Cast<CPrefab>(Object);
            if (Prefab == nullptr)
            {
                LOG_WARN("SpawnPrefab: asset '{}' is not a CPrefab", Path.c_str());
                Callback(entt::null);
                return;
            }

            Callback(Prefab->Instantiate(this, FTransform(), entt::null));
        });
    }

    void CWorld::DuplicateEntity(entt::entity& To, entt::entity From, const TFunctionRef<bool(entt::type_info)>& Callback)
    {
        ASSERT(To != From);

        THashMap<entt::entity, entt::entity> SourceToDuplicate;

        auto DuplicateRecursive = [&](auto& Self, entt::entity Source, entt::entity NewParent) -> entt::entity
        {
            entt::entity NewEntity = EntityRegistry.create();
            SourceToDuplicate[Source] = NewEntity;

            for (auto&& [ID, Storage] : EntityRegistry.storage())
            {
                if (Callback)
                {
                    if (!Callback(Storage.info()))
                    {
                        continue;
                    }
                }

                // Rigid bodies can't be bit-copied; re-emplaced below so on_construct fires fresh.
                if (ID == entt::type_hash<FRelationshipComponent>::value()
                    || ID == entt::type_hash<SRigidBodyComponent>::value())
                {
                    continue;
                }

                if (Storage.contains(Source) && !Storage.contains(NewEntity))
                {
                    Storage.push(NewEntity, Storage.value(Source));
                }
            }

            // Rebind: bit-copy carries source's self-references (Entity/Registry ptr).
            if (STransformComponent* NewTransform = EntityRegistry.try_get<STransformComponent>(NewEntity))
            {
                NewTransform->Bind(EntityRegistry, NewEntity);
                // The copied dirty guards describe the source's queue state; the duplicate is in neither
                // queue, so leaving them set would suppress its own enqueues (transform never resolves,
                // body never teleports).
                NewTransform->ResetDirtyState();
                EntityRegistry.emplace_or_replace<FNeedsTransformUpdate>(NewEntity);
            }

            // Remove auto-emplaced default first; emplace_or_replace would fire on_update (no-op), not on_construct.
            if (const SRigidBodyComponent* SourceBody = EntityRegistry.try_get<SRigidBodyComponent>(Source))
            {
                SRigidBodyComponent NewBody = *SourceBody;
                NewBody.BodyID = 0xFFFFFFFF;

                EntityRegistry.remove<SRigidBodyComponent>(NewEntity);
                EntityRegistry.emplace<SRigidBodyComponent>(NewEntity, eastl::move(NewBody));
            }

            if (NewParent != entt::null)
            {
                ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, NewParent, false);
            }
            else if (FRelationshipComponent* Rel = EntityRegistry.try_get<FRelationshipComponent>(Source))
            {
                if (Rel->Parent != entt::null)
                {
                    ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, Rel->Parent, false);
                }
            }

            ECS::Utils::ForEachChild(EntityRegistry, Source, [&](entt::entity Child)
            {
                Self(Self, Child, NewEntity);
            });

            return NewEntity;
        };

        To = DuplicateRecursive(DuplicateRecursive, From, entt::null);

        for (auto& [Source, Dup] : SourceToDuplicate)
        {
            ECS::Utils::RemapEntityReferences(EntityRegistry, Dup, SourceToDuplicate, /*bClearUnmapped*/ false);
        }
    }

    entt::entity CWorld::DuplicateEntity(entt::entity Source)
    {
        if (Source == entt::null || !EntityRegistry.valid(Source))
        {
            return entt::null;
        }

        entt::entity New = entt::null;
        DuplicateEntity(New, Source, [](entt::type_info) { return true; });
        return New;
    }

    void CWorld::SetParent(entt::entity Child, entt::entity Parent)
    {
        ECS::Utils::ReparentEntity(EntityRegistry, Child, Parent, /*bPreserveWorld*/ true);
    }

    void CWorld::DetachFromParent(entt::entity Entity)
    {
        ECS::Utils::ReparentEntity(EntityRegistry, Entity, entt::null, /*bPreserveWorld*/ true);
    }

    entt::entity CWorld::GetParent(entt::entity Entity)
    {
        const FRelationshipComponent* Relationship = EntityRegistry.try_get<FRelationshipComponent>(Entity);
        return Relationship ? Relationship->Parent : entt::null;
    }

    entt::entity CWorld::GetRootEntity(entt::entity Entity)
    {
        return ECS::Utils::GetRootEntity(EntityRegistry, Entity);
    }

    void CWorld::AttachEntityToSocket(entt::entity Child, entt::entity Parent, const FName& SocketOrBone)
    {
        if (!EntityRegistry.valid(Child) || !EntityRegistry.valid(Parent) || Child == Parent)
        {
            return;
        }

        // Keep the local transform (bPreserveWorld false) -- the socket system overwrites it anyway,
        // and the snap below avoids one frame at the stale local.
        ECS::Utils::ReparentEntity(EntityRegistry, Child, Parent, /*bPreserveWorld*/ false);

        SSocketAttachmentComponent& Attachment = EntityRegistry.emplace_or_replace<SSocketAttachmentComponent>(Child);
        Attachment.SocketName = SocketOrBone;

        FMatrix4 SocketTransform;
        STransformComponent* Transform = EntityRegistry.try_get<STransformComponent>(Child);
        if (Transform && SkeletalUtils::GetEntitySocketTransform(EntityRegistry, Parent, SocketOrBone, SocketTransform))
        {
            Transform->SetLocalTransform(FTransform(SocketTransform * Attachment.RelativeTransform.GetMatrix()));
        }
    }

    void CWorld::DetachEntityFromSocket(entt::entity Entity)
    {
        if (!EntityRegistry.valid(Entity))
        {
            return;
        }

        EntityRegistry.remove<SSocketAttachmentComponent>(Entity);
        ECS::Utils::ReparentEntity(EntityRegistry, Entity, entt::null, /*bPreserveWorld*/ true);
    }

    bool CWorld::HasSocket(entt::entity Entity, const FName& SocketOrBone)
    {
        return SkeletalUtils::EntityHasSocket(EntityRegistry, Entity, SocketOrBone);
    }

    FVector3 CWorld::GetSocketLocation(entt::entity Entity, const FName& SocketOrBone)
    {
        FMatrix4 SocketTransform;
        if (!SkeletalUtils::GetSocketWorldTransform(EntityRegistry, Entity, SocketOrBone, SocketTransform))
        {
            return FVector3(0.0f);
        }
        return FVector3(SocketTransform[3]);
    }

    FQuat CWorld::GetSocketRotation(entt::entity Entity, const FName& SocketOrBone)
    {
        FMatrix4 SocketTransform;
        if (!SkeletalUtils::GetSocketWorldTransform(EntityRegistry, Entity, SocketOrBone, SocketTransform))
        {
            return FQuat::Identity();
        }

        FVector3 Translation; FQuat Rotation; FVector3 Scale;
        AnimPose::DecomposeTRS(SocketTransform, Translation, Rotation, Scale);
        return Rotation;
    }

    FName CWorld::GetBoneName(entt::entity Entity, int32 BoneIndex)
    {
        if (!EntityRegistry.valid(Entity))
        {
            return FName();
        }

        const SSkeletalMeshComponent* Mesh = EntityRegistry.try_get<SSkeletalMeshComponent>(Entity);
        if (Mesh == nullptr)
        {
            return FName();
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        if (Skeleton == nullptr || !Skeleton->IsBoneIndexValid(BoneIndex))
        {
            return FName();
        }
        return Skeleton->GetBone(BoneIndex).Name;
    }

    int32 CWorld::GetBoneIndex(entt::entity Entity, const FName& BoneName)
    {
        if (!EntityRegistry.valid(Entity))
        {
            return INDEX_NONE;
        }

        const SSkeletalMeshComponent* Mesh = EntityRegistry.try_get<SSkeletalMeshComponent>(Entity);
        if (Mesh == nullptr)
        {
            return INDEX_NONE;
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        return Skeleton ? Skeleton->FindBoneIndex(BoneName) : INDEX_NONE;
    }

    FName CWorld::FindClosestBone(entt::entity Entity, FVector3 WorldLocation)
    {
        const int32 BoneIndex = SkeletalUtils::FindClosestBone(EntityRegistry, Entity, WorldLocation);
        return GetBoneName(Entity, BoneIndex);
    }

    void CWorld::DestroyEntity(entt::entity Entity)
    {
        EntityRegistry.destroy(Entity);
    }

    STransformComponent& CWorld::GetEntityTransform(entt::entity Entity)
    {
        return EntityRegistry.get<STransformComponent>(Entity);
    }

    FVector3 CWorld::GetEntityLocation(entt::entity Entity)
    {
        return GetEntityTransform(Entity).GetWorldLocation();
    }

    void CWorld::SetEntityLocation(entt::entity Entity, FVector3 Location)
    {
        GetEntityTransform(Entity).SetLocation(Location);
    }

    void CWorld::SetEntityRotation(entt::entity Entity, FQuat Rotation)
    {
        GetEntityTransform(Entity).SetRotation(Rotation);
    }

    FVector3 CWorld::TranslateEntity(entt::entity Entity, FVector3 Translation)
    {
        return GetEntityTransform(Entity).Translate(Translation);
    }

    uint32 CWorld::GetNumEntities() const
    {
        return (uint32)EntityRegistry.view<entt::entity>().size();
    }

    void CWorld::SetActiveCamera(entt::entity InEntity) const
    {
        SetActiveCamera(InEntity, 0.0f);
    }

    void CWorld::SetActiveCamera(entt::entity InEntity, float BlendTime, ECameraBlendFunction Function) const
    {
        if (!EntityRegistry.valid(InEntity))
        {
            return;
        }

        if (EntityRegistry.all_of<SCameraComponent>(InEntity))
        {
            SCameraSystem::SetActiveCamera(const_cast<FEntityRegistry&>(EntityRegistry), InEntity, BlendTime, Function);
        }
    }

    SCameraComponent* CWorld::GetActiveCamera() const
    {
        return SCameraSystem::GetActiveCamera(const_cast<FEntityRegistry&>(EntityRegistry));
    }

    entt::entity CWorld::GetActiveCameraEntity() const
    {
        return SCameraSystem::GetActiveCameraEntity(const_cast<FEntityRegistry&>(EntityRegistry));
    }

    void CWorld::OnChangeCameraEvent(const FSwitchActiveCameraEvent& Event)
    {
        SetActiveCamera(Event.NewActiveEntity);
    }

    void CWorld::SetTimeDilation(float Dilation)
    {
        GetDefaultWorldSettings().DeltaTimeScale = Math::Max(Dilation, 0.0f);
    }

    float CWorld::GetTimeDilation()
    {
        return GetDefaultWorldSettings().DeltaTimeScale;
    }

    SDefaultWorldSettings& CWorld::GetDefaultWorldSettings()
    {
        if (!EntityRegistry.valid(SingletonEntity))
        {
            static SDefaultWorldSettings Defaults{};
            return Defaults;
        }

        return EntityRegistry.get_or_emplace<SDefaultWorldSettings>(SingletonEntity);
    }

    bool CWorld::EntityHasTag(entt::entity Entity, const FName& Tag)
    {
        if (auto Storage = EntityRegistry.storage(entt::hashed_string(Tag.c_str())))
        {
            return Storage->contains(Entity);
        }
        
        return false;
    }

    void CWorld::CreateRenderer()
    {
        // Headless process or dedicated-server world: no RHI / nothing to display. Leaving RenderScene
        // null makes Extract/Render skip this world (see ExtractWorlds/RenderWorlds and Extract()).
        if (!ShouldRender())
        {
            return;
        }

        if (!RenderScene)
        {
            RenderScene = RenderSceneFactory::Create(this);
            RenderScene->Init();
            EntityRegistry.ctx().emplace<IRenderScene*>(RenderScene.get());
        }
    }

    void CWorld::DestroyRenderer()
    {
        if (RenderScene)
        {
            // Submitted GPU work can still name the scene's resources; the destructor then
            // releases everything it owns.
            if (!GIsHeadless)
            {
                RHI::WaitDeviceIdle();
            }
            RenderScene.reset();
        }
    }

    void CWorld::SetActive(bool bNewActive)
    {
        if (bActive != bNewActive)
        {
            bActive = bNewActive;

            if (bActive)
            {
                SuspendedTime = -1.0;
                CreateRenderer();
                RmlUi::SetActiveWorld(this);
            }
            else
            {
                DestroyRenderer();
            }
        }
    }

    bool CWorld::ReclaimIdleRenderer(double NowSeconds, double GraceSeconds)
    {
        if (bActive || RenderScene == nullptr)
        {
            return false;
        }

        if (SuspendedTime < 0.0)
        {
            // First frame observed idle: start the clock.
            SuspendedTime = NowSeconds;
            return false;
        }

        if (NowSeconds - SuspendedTime < GraceSeconds)
        {
            return false;
        }

        DestroyRenderer();
        return true;
    }

    ENetMode CWorld::GetNetMode() const
    {
        return OwningContext ? OwningContext->NetMode : ENetMode::Standalone;
    }

    bool CWorld::IsNetServer() const
    {
        return NetIsServerMode(GetNetMode());
    }

    int32 CWorld::GetConnectedClientCount() const
    {
        INetworkRuntime* NetRuntime = GetNetworkRuntime();
        return NetRuntime != nullptr ? NetRuntime->GetConnectedClientCount(this) : 0;
    }

    bool CWorld::ShouldRender() const
    {
        // GetNetMode() is valid during InitializeWorld because CreateWorldContext sets OwningContext
        // (and its NetMode) before calling InitializeWorld -- keep that ordering.
        return !GIsHeadless && GetNetMode() != ENetMode::DedicatedServer;
    }

    CWorld* CWorld::DuplicateWorld(CWorld* OwningWorld)
    {
        CPackage* OuterPackage = OwningWorld->GetPackage();
        if (OuterPackage == nullptr)
        {
            return nullptr;
        }

        TVector<uint8> Data;
        FMemoryWriter Writer(Data);
        FObjectProxyArchiver WriterProxy(Writer, true);
        OwningWorld->Serialize(WriterProxy);
        
        FMemoryReader Reader(Data);
        FObjectProxyArchiver ReaderProxy(Reader, true);
        
        CWorld* PIEWorld = NewObject<CWorld>(nullptr, OwningWorld->GetName(), FGuid::New(), OF_Transient);

        PIEWorld->PreLoad();
        PIEWorld->Serialize(ReaderProxy);
        PIEWorld->PostLoad();

        return PIEWorld;
    }

    const TVector<CWorld::FStageSlot>& CWorld::GetSystemsForUpdateStage(EUpdateStage Stage)
    {
        return SystemUpdateList[static_cast<uint32>(Stage)];
    }

    void CWorld::OnRelationshipComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        Registry.on_destroy<FRelationshipComponent>().disconnect<&CWorld::OnRelationshipComponentDestroyed>(this);
        ECS::Utils::RemoveFromParent(Registry, Entity);

        TVector<entt::entity> SubTree;
    
        auto CollectRecursive = [&](auto& Self, entt::entity Current) -> void
        {
            ECS::Utils::ForEachChild(Registry, Current, [&](entt::entity Child)
            {
                Self(Self, Child);
                SubTree.push_back(Child);
            });
        };
    
        CollectRecursive(CollectRecursive, Entity);

        for (int32 i = (int32)SubTree.size() - 1; i >= 0; i--)
        {
            if (Registry.valid(SubTree[i]))
            {
                Registry.destroy(SubTree[i]);
            }
        }
        
        Registry.on_destroy<FRelationshipComponent>().connect<&CWorld::OnRelationshipComponentDestroyed>(this);
    }

    void CWorld::OnTransformComponentConstruct(entt::registry& Registry, entt::entity Entity)
    {
        STransformComponent& TransformComponent = Registry.get<STransformComponent>(Entity);
        TransformComponent.Registry = &EntityRegistry;
        TransformComponent.Entity = Entity;
        TransformComponent.DirtyState = ECS::Utils::EnsureTransformDirtyState(EntityRegistry);

        Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
    }

    void CWorld::OnWidgetComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        RmlUi::ReleaseWidget(this, Registry.get<SWidgetComponent>(Entity));
    }

    void CWorld::OnInputComponentConstruct(entt::registry& Registry, entt::entity Entity)
    {
        Registry.get<SInputComponent>(Entity).World = this;
    }

    void CWorld::OnCSharpScriptComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        SScriptComponent& Component = Registry.get<SScriptComponent>(Entity);
        const int32 Generation = DotNet::GetScriptGeneration();
        for (SScriptInstance& Slot : Component.Scripts)
        {
            if (Slot.Instance != nullptr && Slot.Generation == Generation)
            {
                DotNet::DestroyEntityScript(Slot.Instance);
            }
            Slot.Instance = nullptr;
            Slot.BindState = ECSharpBindState::Unbound;
        }
    }

    void* CWorld::AddEntityScript(entt::entity Entity, FStringView ScriptClass)
    {
        if (!EntityRegistry.valid(Entity) || ScriptClass.empty())
        {
            return nullptr;
        }

        SScriptComponent& Component = EntityRegistry.get_or_emplace<SScriptComponent>(Entity);
        Component.Scripts.emplace_back();
        const int32 NewIndex = (int32)Component.Scripts.size() - 1;
        Component.Scripts[NewIndex].ScriptClass.assign(ScriptClass.data(), ScriptClass.size());

        // Bind now so the caller gets a usable instance; OnReady runs on the next system tick.
        if (!DotNet::IsInitialized())
        {
            return nullptr;
        }
        return BindScriptInstance(reinterpret_cast<uint64>(this), (uint32)entt::to_integral(Entity), Component, NewIndex, DotNet::GetScriptGeneration(), false);
    }

    void CWorld::SetEntityScript(entt::entity Entity, FStringView ScriptClass)
    {
        AddEntityScript(Entity, ScriptClass);
    }

    void CWorld::RegisterSystems()
    {
        DestroyManagedSystems();

        for (int i = 0; i < (int)EUpdateStage::Max; ++i)
        {
            SystemUpdateList[i].clear();
        }
        ActiveSystems.clear();

        for (const FNativeSystemDesc& Desc : FSystemRegistry::Get().GetNativeSystems())
        {
            if (!Desc.Name.IsNone() && DisabledSystems.count(Desc.Name))
            {
                continue;
            }
            
            bool bAnyStage = false;
            for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
            {
                if (!Desc.Priorities.IsStageEnabled((EUpdateStage)i))
                {
                    continue;
                }

                bAnyStage = true;
                if (Desc.Update != nullptr)
                {
                    SystemUpdateList[i].push_back(FStageSlot{ Desc.Update, nullptr, Desc.Access, Desc.Priorities.GetPriorityForStage((EUpdateStage)i), Desc.Name });
                }
            }

            if (bAnyStage)
            {
                FActiveSystem& Active = ActiveSystems.emplace_back();
                Active.Name     = Desc.Name;
                Active.Hash     = Desc.Hash;
                Active.Startup  = Desc.Startup;
                Active.Teardown = Desc.Teardown;
                Active.Self     = nullptr;
            }
        }
        
        ManagedSystemGeneration = DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
        if (DotNet::IsInitialized())
        {
            const int32 Generation = ManagedSystemGeneration;
            TVector<DotNet::FManagedSystemDesc> Descs;
            DotNet::GatherManagedSystemDescs(Descs);

            for (const DotNet::FManagedSystemDesc& Desc : Descs)
            {
                if (Desc.Stage >= EUpdateStage::Max)
                {
                    continue;
                }

                void* Instance = DotNet::CreateManagedSystem(FStringView(Desc.TypeName.c_str(), Desc.TypeName.size()), reinterpret_cast<uint64>(this));
                if (Instance == nullptr)
                {
                    continue;
                }

                FManagedSystem& Managed = ManagedSystems.emplace_back();
                Managed.Instance   = Instance;
                Managed.Stage      = Desc.Stage;
                Managed.Priority   = Desc.Priority;
                Managed.Generation = Generation;

                // A C# system that declared [Reads]/[Writes] access joins the parallel batches exactly like a
                // native system; one with no declared access stays exclusive (the safe default).
                FSystemAccess Access;
                if (Desc.Writes.empty() && Desc.Reads.empty())
                {
                    Access = FSystemAccess::Exclusive();
                }
                else
                {
                    Access.Writes = Desc.Writes;
                    Access.Reads  = Desc.Reads;
                }

                SystemUpdateList[(int32)Desc.Stage].push_back(
                    FStageSlot{ &ManagedSystemUpdate, Instance, Access, (uint8)Desc.Priority, FName(Desc.TypeName.c_str()) });
            }
        }

        // Lower value = higher priority (Highest=0 .. Low=192), so ascending runs Highest first.
        for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
        {
            eastl::sort(SystemUpdateList[i].begin(), SystemUpdateList[i].end(),
                [](const FStageSlot& A, const FStageSlot& B) { return A.StagePriority < B.StagePriority; });
        }
    }

    // Read-only snapshot of how systems group into parallel batches per stage, with each system's declared
    // access. Replays the exact TickSystems greedy batching so the Gameplay Insights editor tool can show the
    // real schedule (replaces the old Core.Systems.LogSchedule console dump). Game thread; cheap.
    namespace
    {
        // List-scheduling: assign each system (already sorted by priority) to the LOWEST-indexed batch whose
        // members it does not conflict with. Unlike the old consecutive batching, this groups mutually
        // non-conflicting systems even when an exclusive/conflicting system sits between them in priority order
        // -- that interleaving was the actual source of "everything runs serial". Batches run in ascending
        // order; because systems are processed in priority order and a conflict pushes a system to a later
        // batch, conflicting pairs still execute in priority order (the higher-priority one first). Returns one
        // index list per batch (indices into Systems).
        TVector<TVector<uint16>> ComputeSystemBatches(const TVector<CWorld::FStageSlot>& Systems)
        {
            TVector<TVector<uint16>> Batches;
            for (uint16 s = 0; s < (uint16)Systems.size(); ++s)
            {
                int32 Chosen = -1;
                for (uint16 b = 0; b < (uint16)Batches.size() && Chosen < 0; ++b)
                {
                    bool bConflicts = false;
                    for (uint16 Member : Batches[b])
                    {
                        if (FSystemAccess::Conflicts(Systems[s].Access, Systems[Member].Access))
                        {
                            bConflicts = true;
                            break;
                        }
                    }
                    if (!bConflicts)
                    {
                        Chosen = (int32)b;
                    }
                }
                if (Chosen < 0)
                {
                    Chosen = (int32)Batches.size();
                    Batches.emplace_back();
                }
                Batches[(size_t)Chosen].push_back(s);
            }
            return Batches;
        }
    }

    void CWorld::GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const
    {
        Out.clear();
        for (uint8 s = 0; s < (uint8)EUpdateStage::Max; ++s)
        {
            const TVector<FStageSlot>& Systems = SystemUpdateList[s];
            const TVector<TVector<uint16>> Batches = ComputeSystemBatches(Systems);
            for (uint8 b = 0; b < (uint8)Batches.size(); ++b)
            {
                for (uint16 Index : Batches[b])
                {
                    const FStageSlot& Slot = Systems[Index];
                    FSystemScheduleEntry& Entry = Out.emplace_back();
                    Entry.Name       = Slot.Name;
                    Entry.Stage      = (uint8)s;
                    Entry.Priority   = Slot.StagePriority;
                    Entry.Batch      = b;
                    Entry.BatchSize  = (uint8)Batches[b].size();
                    Entry.bExclusive = Slot.Access.bExclusive;
                    Entry.bManaged   = Slot.Name.IsNone();
                    Entry.Writes     = Slot.Access.Writes;
                    Entry.Reads      = Slot.Access.Reads;
                }
            }
        }
    }

    void CWorld::DestroyManagedSystems()
    {
        if (ManagedSystems.empty())
        {
            return;
        }

        // After a hot reload the managed side already freed the old generation's GCHandles, so an
        // instance from a stale generation must be DROPPED, not destroyed (that would touch a freed
        // handle, mirroring the SScriptComponent generation guard).
        const int32 Generation = DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
        for (FManagedSystem& Managed : ManagedSystems)
        {
            if (Managed.Instance != nullptr && Managed.Generation == Generation)
            {
                DotNet::DestroyManagedSystem(Managed.Instance);
            }
            Managed.Instance = nullptr;
        }
        ManagedSystems.clear();
    }

    void CWorld::ApplyPendingSystemChanges()
    {
        if (!bSystemsDirty)
        {
            return;
        }
        bSystemsDirty = false;

        const bool bDisabledChanged = (PendingDisabledSystems != DisabledSystems);
        if (!bDisabledChanged)
        {
            return;
        }

        // Snapshot the currently-active unique systems so we can tell which are newly removed/added.
        THashSet<uint64> BeforeHashes;
        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            BeforeHashes.insert(System.Hash);
        });

        // Teardown native systems about to be disabled while their entries are still live (before rebuild).
        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            if (!System.Name.IsNone() && PendingDisabledSystems.count(System.Name) && !DisabledSystems.count(System.Name))
            {
                if (System.Teardown)
                {
                    System.Teardown(System.Self, SystemContext);
                }
            }
        });

        DisabledSystems = PendingDisabledSystems;
        RegisterSystems();

        // Startup systems that are newly present (were not active before the rebuild).
        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            if (BeforeHashes.count(System.Hash) == 0)
            {
                if (System.Startup)
                {
                    System.Startup(System.Self, SystemContext);
                }
            }
        });
    }

    void CWorld::GetAllSystems(TVector<FSystemInfo>& Out) const
    {
        Out.clear();

        for (const FNativeSystemDesc& Desc : FSystemRegistry::Get().GetNativeSystems())
        {
            if (Desc.Name.IsNone())
            {
                continue;
            }

            FSystemInfo& Info = Out.emplace_back();
            Info.Name     = Desc.Name;
            Info.bEnabled = PendingDisabledSystems.count(Info.Name) == 0;

            for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
            {
                if (Desc.Priorities.IsStageEnabled((EUpdateStage)i))
                {
                    Info.Stages.push_back((EUpdateStage)i);
                }
            }
        }
    }

    bool CWorld::IsSystemEnabled(FName System) const
    {
        return PendingDisabledSystems.count(System) == 0;
    }

    void CWorld::SetSystemEnabled(FName System, bool bEnabled)
    {
        if (System.IsNone())
        {
            return;
        }

        const bool bCurrentlyEnabled = PendingDisabledSystems.count(System) == 0;
        if (bCurrentlyEnabled == bEnabled)
        {
            return;
        }

        if (bEnabled)
        {
            PendingDisabledSystems.erase(System);
        }
        else
        {
            PendingDisabledSystems.insert(System);
        }

        // Persist immediately into the world-settings component (safe to mutate; not the live system list).
        // The actual system-list rebuild is deferred to the next frame via ApplyPendingSystemChanges.
        SDefaultWorldSettings& Settings = GetDefaultWorldSettings();
        Settings.DisabledSystems.clear();
        for (const FName& Name : PendingDisabledSystems)
        {
            Settings.DisabledSystems.push_back(Name);
        }

        bSystemsDirty = true;
    }

    void CWorld::DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale)
    {
        if (RenderScene == nullptr)
        {
            return;
        }
        RenderScene->DrawBillboard(ResourceID, Location, Scale);
    }

    void CWorld::DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration)
    {
        if (IsSuspended())
        {
            return;
        }
        
        LineBatcherComponent->EnqueueLine(Start, End, Color, Thickness, bDepthTest, Duration);
    }

    void CWorld::DrawSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode, float Duration)
    {
        if (IsSuspended())
        {
            return;
        }

        TriangleBatcherComponent->EnqueueTriangles(std::move(Vertices), Mode, Duration);
    }

    void CWorld::DrawDebugText(const FString& Text, const FVector4& Color)
    {
#if !defined(LE_SHIPPING)
        if (IsSuspended())
        {
            return;
        }

        FDebugTextLine& Line = DebugTextLines.emplace_back();
        Line.Text  = Text;
        Line.Color = Color;
#endif
    }

    void CWorld::DrainDebugTextLines(TVector<FDebugTextLine>& Out)
    {
        Out = Move(DebugTextLines);
        DebugTextLines.clear();
    }

    TOptional<SRayResult> CWorld::CastRay(const SRayCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();
        
        if (PhysicsScene == nullptr)
        {
            return eastl::nullopt;
        }
        
        TOptional<SRayResult> Result = PhysicsScene->CastRay(Settings);
        
        if (Settings.bDrawDebug)
        {
            if (Result.has_value())
            {
                SRayResult RayResult = Result.value();
                DrawLine(Settings.Start, RayResult.Location, FColor(Settings.DebugMissColor), 3.0f, true, Settings.DebugDuration);
                
                FVector3 NormalEnd = RayResult.Location + RayResult.Normal * 0.5f;
                DrawLine(RayResult.Location, NormalEnd, FColor::Blue, 3.0f,true, Settings.DebugDuration);
                
                DrawBox(RayResult.Location, FVector3(0.05f), FQuat(1.0f, 0.0f, 0.0f, 0.0f), FColor::Yellow, 3.0, true, Settings.DebugDuration);
                
                DrawLine(RayResult.Location, Settings.End, FColor(Settings.DebugHitColor), 3.0f, true, Settings.DebugDuration);
            }
            else
            {
                DrawLine(Settings.Start, Settings.End, FColor(Settings.DebugMissColor), 3.0f, true, Settings.DebugDuration);
            }
        }
        
        return Move(Result);
    }
    
    TVector<SRayResult> CWorld::CastSphere(const SSphereCastSettings& Settings) const
    {
        LUMINA_PROFILE_SCOPE();

        if (PhysicsScene == nullptr)
        {
            return {};
        }
        
        return PhysicsScene->CastSphere(Settings);
        
        
    }

    EUpdateStage CWorld::GetUpdateStage() const
    {
        return SystemContext.GetUpdateStage();
    }

    entt::entity CWorld::GetEntityByTag(const FName& Tag)
    {
        auto& Storage = EntityRegistry.storage<STagComponent>(entt::hashed_string(Tag.c_str()));
        if (Storage.empty())
        {
            return entt::null;
        }
        
        return *Storage.data();
    }

    entt::entity CWorld::GetEntityByName(const FName& Name)
    {
        auto View = EntityRegistry.view<SNameComponent>();
        for (entt::entity Entity : View)
        {
            SNameComponent& NameComponent = View.get<SNameComponent>(Entity);
            if (NameComponent.Name == Name)
            {
                return Entity;
            }
        }
        
        return entt::null;
    }

    FName CWorld::GetEntityName(entt::entity Entity)
    {
        const SNameComponent* Name = EntityRegistry.try_get<SNameComponent>(Entity);
        return Name ? Name->Name : FName();
    }

    entt::entity CWorld::GetFirstEntityWith(entt::id_type Type)
    {
        if (!EntityRegistry.storage(Type))
        {
            return entt::null;
        }

        auto storage = EntityRegistry.storage(Type);

        if (storage->empty())
        {
            return entt::null;
        }
        return *storage->data();
    }

    void CWorld::SetEntityTransform(entt::entity Entity, const FTransform& NewTransform)
    {
        EntityRegistry.emplace_or_replace<STransformComponent>(Entity, NewTransform);
        EntityRegistry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
    }

    void CWorld::TickSystems(FSystemContext& Context)
    {
        TVector<FStageSlot>& Systems = SystemUpdateList[(uint32)Context.GetUpdateStage()];
        
        auto RunOne = [&](FStageSlot& S)
        {
            SetExecutingSystemAccess(&S.Access);
            S.Update(S.Self, Context);
            SetExecutingSystemAccess(nullptr);
        };
        
        const TVector<TVector<uint16>> Batches = ComputeSystemBatches(Systems);
        for (const TVector<uint16>& Batch : Batches)
        {
            if (ECS::Utils::AnyTransformsDirty(EntityRegistry))
            {
                ECS::Utils::ResolveAllDirtyTransforms(EntityRegistry);
            }

            if (Batch.size() == 1)
            {
                RunOne(Systems[Batch[0]]);
            }
            else
            {
                // Create every pool the batch declares BEFORE going wide. entt creates pools lazily inside
                // view(), which writes to the registry's shared pool map -- concurrently with sibling systems
                // reading it. Doing it here on one thread leaves the map immutable for the parallel region.
                for (uint16 Index : Batch)
                {
                    for (void (*Assure)(entt::registry&) : Systems[Index].Access.PoolAssurers)
                    {
                        Assure(EntityRegistry);
                    }
                }

                Task::ParallelFor(static_cast<uint32>(Batch.size()), [&](uint32 Index)
                {
                    DEBUG_ASSERT(!Systems[Batch[Index]].Access.bExclusive); // scheduler invariant: batched => not exclusive
                    RunOne(Systems[Batch[Index]]);
                }, 1);
            }
        }
    }
}
