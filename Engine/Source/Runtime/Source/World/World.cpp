#include "RuntimePCH.h"
#include "World.h"
#include "World/ECS/Registry.h"
#include "World/ECS/EventDispatcher.h"
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
#include "Memory/MemoryTracking.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
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
#include "Entity/Components/ParticleSystemComponent.h"
#include "Entity/Components/StaticMeshComponent.h"
#include "Entity/Components/DynamicMeshComponent.h"
#include "Entity/Components/FoliageComponent.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "Entity/Events/ImpulseEvent.h"
#include "Entity/Components/EntityTags.h"
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
#include "Input/InputQuery.h"
#include "Entity/Components/SingletonEntityComponent.h"
#include "Entity/Components/SceneFolderComponent.h"
#include "Entity/Systems/SystemSingletons.h"
#include "Entity/Systems/CameraSystem.h"
#include "Entity/Components/TagComponent.h"
#include "Entity/Events/WorldEvents.h"
#include "Physics/Physics.h"
#include "Scene/RenderScene/RenderSceneFactory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Net/NetRole.h"
#include "Networking/INetworkRuntime.h"
#include "Subsystems/WorldSettings.h"
#include "UI/RmlUiBridge.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    // Generous for the handful of paint ops a frame issues, and drained every Extract.
    static constexpr uint32 kRenderTargetPaintQueueCapacity = 1024;

    namespace ECS
    {
        // Routes through CWorld's private accessor so the registry stays off the public API.
        ECS::FRegistry& GetWorldRegistry(CWorld& World)
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

        // Both are needed, since one fixes the shared resolve and the other this entity's state.
        template <typename TComponent>
        void MarkMeshResolveDirty(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            Registry.Get<TComponent>(Entity).InvalidateRenderResolve();
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, PrimitiveSourceFor<TComponent>(),
                                                       EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
        }

        // The resolve stamp is irrelevant now; only the render scene needs telling.
        template <typename TComponent>
        void MarkMeshRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, PrimitiveSourceFor<TComponent>(),
                                                       EPrimitiveDirty::Membership);
        }

        void MarkFoliageResolveDirty(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            for (SFoliageType& Type : Registry.Get<SFoliageComponent>(Entity).Types)
            {
                Type.CachedEntryState = MESH_RESOLVE_STATE_STALE;
            }
            FMeshResolveCache::MarkPendingWork();
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage,
                                                       EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
        }

        void MarkFoliageRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Membership);
        }

        // The sync pass drops the sources the entity does not actually have.
        void MarkRenderVisibilityDirty(ECS::FRegistry& Registry, ECS::FEntity Entity)
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

        // One shim forwards every managed tick to the right instance via the .NET host.
        void ManagedSystemUpdate(void* Self, const FSystemContext& Ctx) noexcept
        {
            DotNet::TickManagedSystem(Self, &Ctx);
        }
    }

    //~ World.Debug forwards to this world's draw interface; the draws are no-ops in Shipping.
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
        : SingletonEntity(ECS::NullEntity)
        , SystemContext(this)
        , LineBatcherComponent(nullptr)
        , TriangleBatcherComponent(nullptr)
    {
        DebugInterface.World = this;
        RenderTargetPaintQueue.Initialize(kRenderTargetPaintQueueCapacity);
    }

    void CWorld::EnqueueRenderTargetPaint(FTexturePaintOp&& Op)
    {
        // Dropped rather than spun on, since the drain is a frame away and a lost op is a visual glitch.
        if (!RenderTargetPaintQueue.TryEnqueue(Op))
        {
            LOG_WARN_ONCE("World: the render target paint queue is full; dropping paint operations");
        }
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
        EnqueueRenderTargetPaint(Move(Op));
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
        EnqueueRenderTargetPaint(Move(Op));
    }

    void CWorld::DrainRenderTargetPaints(TVector<FTexturePaintOp>& OutOps)
    {
        FTexturePaintOp Op;
        while (RenderTargetPaintQueue.TryDequeue(Op))
        {
            OutOps.push_back(Move(Op));
        }
    }

    void CWorld::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("World");
        CObject::Serialize(Ar);

        if (Ar.IsReading())
        {
            RegistryPending.Clear();
            ECS::Utils::SerializeRegistry(Ar, RegistryPending);
        }
        else
        {
            // DuplicateWorld serializes pre-init, so write from whichever registry holds the data.
            ECS::FRegistry& Source = (EntityRegistry.NumEntities() != 0)
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
        LUMINA_MEMORY_SCOPE("World");
        WorldType = InWorldType;
        
        CPrefab::CullOrphanedInstances(RegistryPending);
        
        if (RegistryPending.NumEntities() != 0)
        {
            EntityRegistry.Swap(RegistryPending);
        }
        RegistryPending = {};

        CPrefab::RefreshAllInstancesInWorld(this);
        
        EntityRegistry.Compact();
        
        // Which entities a client may hold is a netcode question, so it is reported not decided.
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->OnWorldEntitiesLoaded(this);
        }

        EntityRegistry.Ctx().Emplace<ECS::FEventDispatcher*>(&SingletonDispatcher);

        ConnectComponentAccessValidators(EntityRegistry);

        auto WorldSettingsView = EntityRegistry.View<SDefaultWorldSettings>();
        for (auto Entity : WorldSettingsView)
        {
            if (!ALERT_IF_NOT(WorldSettingsView.Num() == 1, "Multiple world settings were detected in the world! {}", WorldSettingsView.Num()))
            {
                EntityRegistry.ClearComponent<SDefaultWorldSettings>();
                break;
            }
            
            SingletonEntity = Entity;
            break;
        }
        
        if (!EntityRegistry.IsValid(SingletonEntity))
        {
            SingletonEntity = EntityRegistry.Create();
            EntityRegistry.Emplace<SDefaultWorldSettings>(SingletonEntity);
        }
        
        LineBatcherComponent = &EntityRegistry.Emplace<FLineBatcherComponent>(SingletonEntity);
        TriangleBatcherComponent = &EntityRegistry.Emplace<FTriangleBatcherComponent>(SingletonEntity);
        EntityRegistry.Emplace<FSingletonEntityTag>(SingletonEntity);
        EntityRegistry.Emplace<FHideInSceneOutliner>(SingletonEntity);
        
        // Physics scene only for simulating worlds; the world reserves its body arrays up front.
        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            PhysicsScene = Physics::GetPhysicsContext()->CreatePhysicsScene(this);
        }
        // Emplaced even when null so ctx().get consumers find the key and null-check it.
        EntityRegistry.Ctx().Emplace<Physics::IPhysicsScene*>(PhysicsScene.get());
        EntityRegistry.Ctx().Emplace<FSystemContext*>(&SystemContext);
        EntityRegistry.Ctx().Emplace<CWorld*>(this);

        // Per-world subsystem singleton ticked by STimerSystem and reached by ctx address.
        EntityRegistry.Ctx().Emplace<FTimerManager>();

        // SCameraSystem owns FCameraGlobalState and writes FResolvedSceneView for Extract.
        EntityRegistry.Ctx().Emplace<FCameraGlobalState>();
        EntityRegistry.Ctx().Emplace<FResolvedSceneView>();

        CreateRenderer();
        UIContext = RmlUi::CreateWorldUI(this);

        // Seeded before registering systems so a disabled system is never constructed.
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

        bSystemsStarted = true;
        StartupManagedSystems();

        EntityRegistry.GetSignals<FRelationshipComponent>().OnDestroy      .Connect<&ThisClass::OnRelationshipComponentDestroyed>(this);
        EntityRegistry.GetSignals<STransformComponent>().OnConstruct         .Connect<&ThisClass::OnTransformComponentConstruct>(this);
        EntityRegistry.GetSignals<FRelationshipComponent>().OnConstruct      .Connect<&ThisClass::OnRelationshipComponentConstruct>(this);
        EntityRegistry.GetSignals<SEntityScriptComponent>().OnDestroy      .Connect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityRegistry.GetSignals<SWidgetComponent>().OnDestroy            .Connect<&ThisClass::OnWidgetComponentDestroyed>(this);
        SystemContext.EventSink     <FSwitchActiveCameraEvent>()    .Connect<&ThisClass::OnChangeCameraEvent>(this);

        // on_destroy is what keeps the persistent primitive table from outliving its source.
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnConstruct  .Connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnUpdate  .Connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnDestroy  .Connect<&MarkMeshRemoved<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnConstruct .Connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnUpdate .Connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnDestroy .Connect<&MarkMeshRemoved<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnConstruct.Connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnUpdate.Connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnDestroy.Connect<&MarkMeshRemoved<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnConstruct     .Connect<&MarkFoliageResolveDirty>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnUpdate     .Connect<&MarkFoliageResolveDirty>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnDestroy     .Connect<&MarkFoliageRemoved>();
        EntityRegistry.GetSignals<SDisabledTag>().OnConstruct          .Connect<&MarkRenderVisibilityDirty>();
        EntityRegistry.GetSignals<SDisabledTag>().OnDestroy          .Connect<&MarkRenderVisibilityDirty>();

        // Components loaded before these hooks connected never saw on_construct.
        FMeshResolveCache::MarkPendingWork();
        // A full rescan is the only way to pick up primitives that predate the hooks.
        FRenderDirtyTracker::Ensure(EntityRegistry).RequestFullRescan();

        ECS::Utils::FTransformDirtyGate* DirtyState = ECS::Utils::EnsureTransformDirtyGate(EntityRegistry);
        auto TransformView = EntityRegistry.View<STransformComponent>();
        TransformView.ForEach([&](ECS::FEntity Entity, STransformComponent& TransformComponent)
        {
            TransformComponent.Registry = &EntityRegistry;
            TransformComponent.Entity = Entity;
            TransformComponent.DirtyState = DirtyState;
        });

        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            const auto AnyCameraView = EntityRegistry.View<SCameraComponent>();
            if (AnyCameraView.begin() == AnyCameraView.end())
            {
                LOG_WARN("CWorld::Initialize: world '{}' has no camera entity; spawning a fallback at (0, 2, 5) looking at origin. Add a camera entity for proper gameplay.", GetName());

                constexpr FVector3 FallbackPos(0.0f, 2.0f, 5.0f);

                const ECS::FEntity Fallback = EntityRegistry.Create();
                STransformComponent& Xf = EntityRegistry.Emplace<STransformComponent>(Fallback);
                Xf.LocalTransform.SetLocation(FallbackPos);
                Xf.LocalTransform.SetRotation(Math::FindLookAtRotation(FVector3(0.0f), FallbackPos));

                SCameraComponent& Cam = EntityRegistry.Emplace<SCameraComponent>(Fallback);
                Cam.bAutoActivate = true;
            }
        }

        auto CameraView = EntityRegistry.View<SCameraComponent>(ECS::TExclude<SDisabledTag>{});
        CameraView.ForEach([&](ECS::FEntity Entity, const SCameraComponent& Camera)
        {
           if (Camera.bAutoActivate)
           {
               SingletonDispatcher.Trigger<FSwitchActiveCameraEvent>(FSwitchActiveCameraEvent{Entity});
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
        // First, so every subscription the managed side drops still disconnects against a live world.
        DotNet::NotifyWorldTeardown(this);

        // No render phase / RHI / audio device in a headless process.
        if (!GIsHeadless)
        {
            RHI::WaitDeviceIdle();
        }

        // The viewport outlives the world, so an unpopped layer would gate input in the next PIE.
        Input::ClearLayers(this);

        Audio::Context().StopAllSounds();

        EntityRegistry.GetSignals<FRelationshipComponent>().OnDestroy.Disconnect<&ThisClass::OnRelationshipComponentDestroyed>(this);

        ForEachUniqueSystem([&](const FActiveSystem& System)
        {
            if (System.Teardown)
            {
                System.Teardown(System.Self, SystemContext);
            }
        });

        // Release this world's C# system instances (OnTeardown + GCHandle free).
        DestroyManagedSystems();

        // Keyed on the scene, since StopSimulate is what disconnects its registry listeners.
        if (PhysicsScene != nullptr)
        {
            PhysicsScene->StopSimulate();
        }

        EntityRegistry.Ctx().Get<FTimerManager>().Clear();

        // Detached up front, since OnDestroy publishes while iterating the pool it is about to empty.
        EntityRegistry.GetSignals<SEntityScriptComponent>().OnDestroy.Disconnect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityScripts::DetachAllInRegistry(EntityRegistry);

        RegistryPending.Clear();
        EntityRegistry.Clear();

        // After the registry, since clearing it runs OnDetach on the context this destroys.
        RmlUi::DestroyWorldUI(this);
        UIContext.reset();

        PhysicsScene.reset();
        DestroyRenderer();

        FCoreDelegates::PostWorldUnload.Broadcast();
    }

    void CWorld::Update(const FUpdateContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        const EUpdateStage Stage = Context.GetUpdateStage();

        // Applied between frames, so a mid-frame toggle never lands inside a running batch.
        ApplyPendingSystemChanges();

        // Rebuild before any tick so the shared shim never dereferences a freed GCHandle.
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

        if ((bPaused && Stage != EUpdateStage::Paused) || (!bPaused && Stage == EUpdateStage::Paused))
        {
            return;
        }

        SystemContext.DeltaTime     = DeltaTime;
        SystemContext.Time          = TimeSinceCreation;
        SystemContext.UpdateStage   = Stage;

        // Deferred timers run inside TickSystems now, still before gameplay systems.
        TickSystems(SystemContext);
    }

    Physics::IPhysicsScene* CWorld::EnsurePhysicsScene()
    {
        if (PhysicsScene == nullptr)
        {
            PhysicsScene = Physics::GetPhysicsContext()->CreatePhysicsScene(this);

            // Simulate() connects the hook that turns SRigidBodyComponent into an actual body.
            PhysicsScene->Simulate();
        }

        return PhysicsScene.get();
    }

    void CWorld::TickPhysics()
    {
        LUMINA_PROFILE_SCOPE();

        if (bPaused || PhysicsScene == nullptr)
        {
            return;
        }

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
        
        const FResolvedSceneView& View = EntityRegistry.Ctx().Get<FResolvedSceneView>();

        if (View.bHasView)
        {
            RenderScene->SetActivePostProcessMaterials(View.PostProcessMaterials);
            RenderScene->Extract(View.ViewVolume, View.bHasPostProcess ? &View.PostProcess : nullptr);
            return;
        }

        RenderScene->SetActivePostProcessMaterials({});
        RenderScene->Extract(FViewVolume{}, nullptr);
    }

    ECS::FEntity CWorld::ConstructEntity(FName Name, const FTransform& Transform)
    {
        DEBUG_ASSERT(Threading::IsMainThread(), "You may only construct entities on the main thread.");
        
        ECS::FEntity NewEntity = GetEntityRegistry().Create();
        
        if (Name == NAME_None)
        {
            Name = FName("Entity", (NewEntity).Value);
        }
     
        EntityRegistry.Emplace<SNameComponent>(NewEntity, Name);
        EntityRegistry.Emplace<STransformComponent>(NewEntity, Transform);

        return NewEntity;
    }

    ECS::FEntity CWorld::SpawnProjectile(FVector3 Position, FVector3 Velocity, float Damage, float Lifetime, ECS::FEntity Instigator)
    {
        FTransform SpawnTransform;
        SpawnTransform.SetLocation(Position);
        ECS::FEntity Entity = ConstructEntity("Projectile", SpawnTransform);

        SProjectileComponent& Projectile = EntityRegistry.Emplace<SProjectileComponent>(Entity);
        Projectile.Velocity = Velocity;
        Projectile.Damage = Damage;
        Projectile.Instigator = Instigator;

        // Reuse the engine lifetime system for auto-despawn.
        if (Lifetime > 0.0f)
        {
            EntityRegistry.Emplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
        }
        return Entity;
    }

    bool CWorld::FractureEntity(ECS::FEntity Entity, const FVector3& Origin, float Strength)
    {
        LUMINA_PROFILE_SCOPE();

        if (!EntityRegistry.IsValid(Entity))
        {
            return false;
        }

        SDestructibleComponent* Destructible = EntityRegistry.TryGet<SDestructibleComponent>(Entity);
        if (Destructible == nullptr || Destructible->bFractured)
        {
            return false;
        }

        // Resolve the mesh to shatter, taking an explicit override before the entity's own mesh.
        SStaticMeshComponent* MeshComp = EntityRegistry.TryGet<SStaticMeshComponent>(Entity);
        CStaticMesh* SourceMesh = Destructible->FragmentMesh.Get();
        if (SourceMesh == nullptr && MeshComp != nullptr)
        {
            SourceMesh = MeshComp->StaticMesh.Get();
        }

        if (SourceMesh == nullptr)
        {
            LOG_WARN("FractureEntity: entity {} has no mesh to fracture", (Entity).Value);
            return false;
        }

        FTransform OwnerTransform = EntityRegistry.Get<STransformComponent>(Entity).GetWorldTransform();
        
        FVector3 InheritedVelocity(0.0f);
        if (PhysicsScene)
        {
            if (const SRigidBodyComponent* RB = EntityRegistry.TryGet<SRigidBodyComponent>(Entity))
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

        // Deterministic per-fragment jitter from the index hash, which replays and lockstep need.
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

        // An assigned collection if present, else a convex Voronoi fracture from the mesh bounds.
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
            Settings.Seed      = (Entity).Value * 2654435761U + 1u;
            Fracture::GenerateConvexFracture(SourceMesh, Settings, GeneratedPieces);
        }

        const TVector<FFracturePiece>& Pieces = CollectionData ? CollectionData->Pieces : GeneratedPieces;

        // BodyIDs are valid only after EndBodyBatch, so impulses are collected and applied then.
        struct FPendingLaunch { ECS::FEntity Fragment; FVector3 Center; uint32 Seed; };
        TVector<FPendingLaunch> PendingLaunches;
        PendingLaunches.reserve(Pieces.size());

        // Cap fragments at physics body headroom, since overflowing the body arrays trips a hard assert.
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

            // Pre-baked collections cache piece meshes; the Voronoi path builds each one inline.
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

                // BuildPieceMesh recenters to the piece centroid, so place the entity at that world position.
                const FVector3 WorldCenter = OwnerTransform.GetLocation() + OwnerTransform.GetRotation() * (OwnerTransform.GetScale() * Piece.Center);
                FTransform PieceTransform;
                PieceTransform.SetLocation(WorldCenter);
                PieceTransform.SetRotation(OwnerTransform.GetRotation());
                PieceTransform.SetScale(OwnerTransform.GetScale());

                const ECS::FEntity Fragment = ConstructEntity("Fragment", PieceTransform);
                EntityRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(Fragment);

                EntityRegistry.Emplace<SStaticMeshComponent>(Fragment).SetStaticMesh(PieceMesh);

                // The collider's on_construct builds the shape synchronously, so set Mesh and bConvex first.
                SMeshColliderComponent ColliderDesc;
                ColliderDesc.Mesh    = PieceMesh;
                ColliderDesc.bConvex = true;
                EntityRegistry.Emplace<SMeshColliderComponent>(Fragment, std::move(ColliderDesc));

                EntityRegistry.Emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.Emplace<SFragmentComponent>(Fragment).Source   = (Entity).Value;
                
                PendingLaunches.push_back({ Fragment, WorldCenter, (Fragment).Value + static_cast<uint32>(Spawned) });

                ++Spawned;
            }
        }
        else
        {
            // Degenerate fracture falls back to subdividing the bounds into textured box chunks.
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

                const ECS::FEntity Fragment = ConstructEntity("Fragment", FragmentTransform);
                EntityRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(Fragment);

                SStaticMeshComponent& FragmentMeshComp = EntityRegistry.Emplace<SStaticMeshComponent>(Fragment);
                FragmentMeshComp.SetStaticMesh(GridMesh);
                if (MeshComp != nullptr)
                {
                    FragmentMeshComp.MaterialOverrides = MeshComp->MaterialOverrides;
                }

                // The box collider builds a Dynamic body synchronously, so set HalfExtent up front.
                SBoxColliderComponent BoxDesc;
                BoxDesc.HalfExtent = ColliderHalf;
                EntityRegistry.Emplace<SBoxColliderComponent>(Fragment, std::move(BoxDesc));

                EntityRegistry.Emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.Emplace<SFragmentComponent>(Fragment).Source   = (Entity).Value;

                PendingLaunches.push_back({ Fragment, CellWorldCenter, (Fragment).Value + static_cast<uint32>(Spawned) });

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
            LaunchBody(EntityRegistry.Get<SRigidBodyComponent>(Launch.Fragment).BodyID, Launch.Center, Launch.Seed);
        }

        Destructible->bFractured = true;

        // Strip render and physics now so it vanishes this frame; the lifetime system reaps it.
        if (Destructible->bDestroyOriginal)
        {
            EntityRegistry.Remove<SStaticMeshComponent>(Entity);
            EntityRegistry.Remove<SRigidBodyComponent>(Entity);
            EntityRegistry.Remove<SBoxColliderComponent>(Entity);
            EntityRegistry.Remove<SSphereColliderComponent>(Entity);
            EntityRegistry.Remove<SMeshColliderComponent>(Entity);
            EntityRegistry.EmplaceOrReplace<SLifetimeComponent>(Entity).Lifetime = 0.01f;
        }

        return Spawned > 0;
    }

    ECS::FEntity CWorld::SpawnPrefab(const FAssetRef& Prefab)
    {
        return SpawnPrefabAt(Prefab, FTransform(), ECS::NullEntity);
    }

    ECS::FEntity CWorld::SpawnPrefabAt(const FAssetRef& Prefab, const FTransform& SpawnTransform, ECS::FEntity Parent)
    {
        FStringView Path = Prefab.GetPath();
        FAssetData* AssetData = FAssetRegistry::Get().GetAssetByPath(Path);
        if (AssetData == nullptr)
        {
            LOG_WARN("SpawnPrefab: no asset found at path '{}'", Path);
            return ECS::NullEntity;
        }

        // A cold spawn fans the prefab's closure across the swarm; a resident one takes the lookup.
        CPrefab* PrefabObject = FindObject<CPrefab>(AssetData->AssetGUID);
        if (PrefabObject == nullptr)
        {
            PrefabObject = LoadObjectGraph<CPrefab>(AssetData->AssetGUID);
        }
        if (PrefabObject == nullptr)
        {
            LOG_WARN("SpawnPrefab: asset '{}' is not a CPrefab", Path);
            return ECS::NullEntity;
        }

        return PrefabObject->Instantiate(this, SpawnTransform, Parent);
    }

    ECS::FEntity CWorld::SpawnParticleSystem(CParticleSystem* ParticleSystem, const FTransform& SpawnTransform, float Lifetime)
    {
        if (ParticleSystem == nullptr)
        {
            return ECS::NullEntity;
        }

        const ECS::FEntity Spawned = ConstructEntity("ParticleEffect", SpawnTransform);
        SParticleSystemComponent& Effect = EmplaceComponent<SParticleSystemComponent>(Spawned);
        Effect.ParticleSystem = ParticleSystem;
        Effect.bBurstOnSpawn = true;
        Effect.Activate(true);

        SetEntityLifetime(Spawned, Lifetime);
        return Spawned;
    }

    ECS::FEntity CWorld::SpawnParticleSystemAttached(CParticleSystem* ParticleSystem, ECS::FEntity Parent,
        const FName& Socket, FVector3 Offset, float Lifetime)
    {
        if (ParticleSystem == nullptr || !IsValidEntity(Parent))
        {
            return ECS::NullEntity;
        }

        // Attaching snaps the child onto the socket, so spawning at a world point first would be undone.
        const ECS::FEntity Spawned = ConstructEntity("ParticleEffect", FTransform());
        SParticleSystemComponent& Effect = EmplaceComponent<SParticleSystemComponent>(Spawned);
        Effect.ParticleSystem = ParticleSystem;
        Effect.EmitterOffset = Offset;
        Effect.bBurstOnSpawn = true;
        Effect.Activate(true);

        // A managed caller spells "no socket" as an empty string, which does not intern to NAME_None.
        if (Socket.IsNone() || FStringView(Socket.c_str()).empty())
        {
            SetParent(Spawned, Parent);
        }
        else
        {
            AttachEntityToSocket(Spawned, Parent, Socket);
        }

        SetEntityLifetime(Spawned, Lifetime);
        return Spawned;
    }

    void CWorld::SetEntityLifetime(ECS::FEntity Entity, float Seconds)
    {
        if (Seconds > 0.0f && IsValidEntity(Entity))
        {
            GetOrEmplaceComponent<SLifetimeComponent>(Entity).Lifetime = Seconds;
        }
    }

    void CWorld::SpawnPrefabAsync(const FName& Path, const TFunction<void(ECS::FEntity)>& Callback)
    {
        AsyncLoadObject(Path, [this, Callback, Path](CObject* Object)
        {
            CPrefab* Prefab = Cast<CPrefab>(Object);
            if (Prefab == nullptr)
            {
                LOG_WARN("SpawnPrefab: asset '{}' is not a CPrefab", Path.c_str());
                Callback(ECS::NullEntity);
                return;
            }

            Callback(Prefab->Instantiate(this, FTransform(), ECS::NullEntity));
        });
    }

    void CWorld::DuplicateEntity(ECS::FEntity& To, ECS::FEntity From, const TFunctionRef<bool(const ECS::FComponentTypeInfo&)>& Callback)
    {
        LUMINA_MEMORY_SCOPE("World");
        ASSERT(To != From);

        THashMap<ECS::FEntity, ECS::FEntity> SourceToDuplicate;

        auto DuplicateRecursive = [&](auto& Self, ECS::FEntity Source, ECS::FEntity NewParent) -> ECS::FEntity
        {
            ECS::FEntity NewEntity = EntityRegistry.Create();
            SourceToDuplicate[Source] = NewEntity;

            for (Lumina::ECS::FSparseSet* StoragePtr : EntityRegistry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (Callback)
                {
                    if (!Callback(Storage.GetTypeInfo()))
                    {
                        continue;
                    }
                }

                // Rigid bodies can't be bit-copied; re-emplaced below so on_construct fires fresh.
                if (ID == ECS::GetComponentTypeID<FRelationshipComponent>()
                    || ID == ECS::GetComponentTypeID<SRigidBodyComponent>())
                {
                    continue;
                }

                if (Storage.Contains(Source) && !Storage.Contains(NewEntity))
                {
                    Storage.EmplaceCopyRaw(NewEntity, Storage.GetRaw(Source));
                }
            }

            // Rebind, since a bit-copy carries the source's self-references.
            if (STransformComponent* NewTransform = EntityRegistry.TryGet<STransformComponent>(NewEntity))
            {
                NewTransform->Bind(EntityRegistry, NewEntity);
                // The duplicate is in neither queue, so a copied dirty guard would suppress its enqueues.
                NewTransform->ResetDirtyState();
                EntityRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(NewEntity);
            }

            // Remove auto-emplaced default first; emplace_or_replace would fire on_update (no-op), not on_construct.
            if (const SRigidBodyComponent* SourceBody = EntityRegistry.TryGet<SRigidBodyComponent>(Source))
            {
                SRigidBodyComponent NewBody = *SourceBody;
                NewBody.BodyID = 0xFFFFFFFF;

                EntityRegistry.Remove<SRigidBodyComponent>(NewEntity);
                EntityRegistry.Emplace<SRigidBodyComponent>(NewEntity, std::move(NewBody));
            }

            if (NewParent != ECS::NullEntity)
            {
                ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, NewParent, false);
            }
            else if (FRelationshipComponent* Rel = EntityRegistry.TryGet<FRelationshipComponent>(Source))
            {
                if (Rel->Parent != ECS::NullEntity)
                {
                    ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, Rel->Parent, false);
                }
            }

            ECS::Utils::ForEachChild(EntityRegistry, Source, [&](ECS::FEntity Child)
            {
                Self(Self, Child, NewEntity);
            });

            return NewEntity;
        };

        To = DuplicateRecursive(DuplicateRecursive, From, ECS::NullEntity);

        for (auto& [Source, Dup] : SourceToDuplicate)
        {
            ECS::Utils::RemapEntityReferences(EntityRegistry, Dup, SourceToDuplicate, /*bClearUnmapped*/ false);
        }
    }

    ECS::FEntity CWorld::DuplicateEntity(ECS::FEntity Source)
    {
        if (Source == ECS::NullEntity || !EntityRegistry.IsValid(Source))
        {
            return ECS::NullEntity;
        }

        ECS::FEntity New = ECS::NullEntity;
        DuplicateEntity(New, Source, [](ECS::FComponentTypeInfo) { return true; });
        return New;
    }

    void CWorld::SetParent(ECS::FEntity Child, ECS::FEntity Parent)
    {
        ECS::Utils::ReparentEntity(EntityRegistry, Child, Parent, /*bPreserveWorld*/ true);
    }

    void CWorld::DetachFromParent(ECS::FEntity Entity)
    {
        ECS::Utils::ReparentEntity(EntityRegistry, Entity, ECS::NullEntity, /*bPreserveWorld*/ true);
    }

    ECS::FEntity CWorld::GetParent(ECS::FEntity Entity)
    {
        const FRelationshipComponent* Relationship = EntityRegistry.TryGet<FRelationshipComponent>(Entity);
        return Relationship ? Relationship->Parent : ECS::NullEntity;
    }

    ECS::FEntity CWorld::GetRootEntity(ECS::FEntity Entity)
    {
        return ECS::Utils::GetRootEntity(EntityRegistry, Entity);
    }

    void CWorld::AttachEntityToSocket(ECS::FEntity Child, ECS::FEntity Parent, const FName& SocketOrBone)
    {
        if (!EntityRegistry.IsValid(Child) || !EntityRegistry.IsValid(Parent) || Child == Parent)
        {
            return;
        }

        // The socket system overwrites the local transform anyway, and the snap avoids a stale frame.
        ECS::Utils::ReparentEntity(EntityRegistry, Child, Parent, /*bPreserveWorld*/ false);

        SSocketAttachmentComponent& Attachment = EntityRegistry.EmplaceOrReplace<SSocketAttachmentComponent>(Child);
        Attachment.SocketName = SocketOrBone;

        FMatrix4 SocketTransform;
        STransformComponent* Transform = EntityRegistry.TryGet<STransformComponent>(Child);
        if (Transform && SkeletalUtils::GetEntitySocketTransform(EntityRegistry, Parent, SocketOrBone, SocketTransform))
        {
            Transform->SetLocalTransform(FTransform(SocketTransform * Attachment.RelativeTransform.GetMatrix()));
        }
    }

    void CWorld::DetachEntityFromSocket(ECS::FEntity Entity)
    {
        if (!EntityRegistry.IsValid(Entity))
        {
            return;
        }

        EntityRegistry.Remove<SSocketAttachmentComponent>(Entity);
        ECS::Utils::ReparentEntity(EntityRegistry, Entity, ECS::NullEntity, /*bPreserveWorld*/ true);
    }

    bool CWorld::HasSocket(ECS::FEntity Entity, const FName& SocketOrBone)
    {
        return SkeletalUtils::EntityHasSocket(EntityRegistry, Entity, SocketOrBone);
    }

    FVector3 CWorld::GetSocketLocation(ECS::FEntity Entity, const FName& SocketOrBone)
    {
        FMatrix4 SocketTransform;
        if (!SkeletalUtils::GetSocketWorldTransform(EntityRegistry, Entity, SocketOrBone, SocketTransform))
        {
            return FVector3(0.0f);
        }
        return FVector3(SocketTransform[3]);
    }

    FQuat CWorld::GetSocketRotation(ECS::FEntity Entity, const FName& SocketOrBone)
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

    FName CWorld::GetBoneName(ECS::FEntity Entity, int32 BoneIndex)
    {
        if (!EntityRegistry.IsValid(Entity))
        {
            return FName();
        }

        const SSkeletalMeshComponent* Mesh = EntityRegistry.TryGet<SSkeletalMeshComponent>(Entity);
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

    int32 CWorld::GetBoneIndex(ECS::FEntity Entity, const FName& BoneName)
    {
        if (!EntityRegistry.IsValid(Entity))
        {
            return INDEX_NONE;
        }

        const SSkeletalMeshComponent* Mesh = EntityRegistry.TryGet<SSkeletalMeshComponent>(Entity);
        if (Mesh == nullptr)
        {
            return INDEX_NONE;
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        return Skeleton ? Skeleton->FindBoneIndex(BoneName) : INDEX_NONE;
    }

    FName CWorld::FindClosestBone(ECS::FEntity Entity, FVector3 WorldLocation)
    {
        const int32 BoneIndex = SkeletalUtils::FindClosestBone(EntityRegistry, Entity, WorldLocation);
        return GetBoneName(Entity, BoneIndex);
    }

    void CWorld::DestroyEntity(ECS::FEntity Entity)
    {
        EntityRegistry.Destroy(Entity);
    }

    STransformComponent& CWorld::GetEntityTransform(ECS::FEntity Entity)
    {
        return EntityRegistry.Get<STransformComponent>(Entity);
    }

    FVector3 CWorld::GetEntityLocation(ECS::FEntity Entity)
    {
        return GetEntityTransform(Entity).GetWorldLocation();
    }

    void CWorld::SetEntityLocation(ECS::FEntity Entity, FVector3 Location)
    {
        GetEntityTransform(Entity).SetLocation(Location);
    }

    void CWorld::SetEntityRotation(ECS::FEntity Entity, FQuat Rotation)
    {
        GetEntityTransform(Entity).SetRotation(Rotation);
    }

    FVector3 CWorld::TranslateEntity(ECS::FEntity Entity, FVector3 Translation)
    {
        return GetEntityTransform(Entity).Translate(Translation);
    }

    uint32 CWorld::GetNumEntities() const
    {
        return (uint32)EntityRegistry.NumEntities();
    }

    void CWorld::SetActiveCamera(ECS::FEntity InEntity) const
    {
        SetActiveCamera(InEntity, 0.0f);
    }

    void CWorld::SetActiveCamera(ECS::FEntity InEntity, float BlendTime, ECameraBlendFunction Function) const
    {
        if (!EntityRegistry.IsValid(InEntity))
        {
            return;
        }

        if (EntityRegistry.HasAll<SCameraComponent>(InEntity))
        {
            SCameraSystem::SetActiveCamera(const_cast<ECS::FRegistry&>(EntityRegistry), InEntity, BlendTime, Function);
        }
    }

    SCameraComponent* CWorld::GetActiveCamera() const
    {
        return SCameraSystem::GetActiveCamera(const_cast<ECS::FRegistry&>(EntityRegistry));
    }

    ECS::FEntity CWorld::GetActiveCameraEntity() const
    {
        return SCameraSystem::GetActiveCameraEntity(const_cast<ECS::FRegistry&>(EntityRegistry));
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
        if (!EntityRegistry.IsValid(SingletonEntity))
        {
            static SDefaultWorldSettings Defaults{};
            return Defaults;
        }

        return EntityRegistry.GetOrEmplace<SDefaultWorldSettings>(SingletonEntity);
    }

    SSceneFolderComponent& CWorld::GetSceneFolders()
    {
        if (!EntityRegistry.IsValid(SingletonEntity))
        {
            static SSceneFolderComponent Empty{};
            return Empty;
        }

        return EntityRegistry.GetOrEmplace<SSceneFolderComponent>(SingletonEntity);
    }

    SSceneFolderComponent* CWorld::FindSceneFolders()
    {
        if (!EntityRegistry.IsValid(SingletonEntity))
        {
            return nullptr;
        }

        return EntityRegistry.TryGet<SSceneFolderComponent>(SingletonEntity);
    }

    const SSceneFolderComponent* CWorld::FindSceneFolders() const
    {
        if (!EntityRegistry.IsValid(SingletonEntity))
        {
            return nullptr;
        }

        return EntityRegistry.TryGet<SSceneFolderComponent>(SingletonEntity);
    }

    bool CWorld::EntityHasTag(ECS::FEntity Entity, const FName& Tag)
    {
        if (const ECS::FSparseSet* Storage = EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag))
        {
            return Storage->Contains(Entity);
        }
        
        return false;
    }

    void CWorld::CreateRenderer()
    {
        // A null RenderScene makes Extract and Render skip this world.
        if (!ShouldRender())
        {
            return;
        }

        if (!RenderScene)
        {
            RenderScene = RenderSceneFactory::Create(this);
            RenderScene->Init();
            EntityRegistry.Ctx().Emplace<IRenderScene*>(RenderScene.get());
        }
    }

    void CWorld::DestroyRenderer()
    {
        if (RenderScene)
        {
            // Submitted GPU work can still name the scene's resources.
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
                // Back before the grace ran out, and -1 re-arms the idle clock ReclaimIdleRenderer stamps.
                SuspendedTime = -1.0;
                CreateRenderer();
                RmlUi::SetActiveWorld(this);
            }
        }
    }

    void CWorld::SetUpdateInterval(double Seconds)
    {
        const double Clamped = Seconds > 0.0 ? Seconds : 0.0;
        if (UpdateIntervalSeconds == Clamped)
        {
            return;
        }

        UpdateIntervalSeconds = Clamped;

        // A deadline in the past means eligible now, so a clicked-into throttled tool draws at once.
        NextUpdateTime      = 0.0;
        bThrottledThisFrame = false;
    }

    void CWorld::AdvanceThrottle(double NowSeconds)
    {
        if (UpdateIntervalSeconds <= 0.0 || !bActive)
        {
            bThrottledThisFrame = false;
            NextUpdateTime      = 0.0;
            return;
        }

        bThrottledThisFrame = (NowSeconds < NextUpdateTime);
        if (!bThrottledThisFrame)
        {
            // Anchored to NOW, or a long-suspended world comes back owing a burst of catch-up frames.
            NextUpdateTime = NowSeconds + UpdateIntervalSeconds;
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
            // First frame observed idle, so start the clock.
            SuspendedTime = NowSeconds;
            return false;
        }

        if (NowSeconds - SuspendedTime < GraceSeconds)
        {
            return false;
        }

        LUMINA_PROFILE_SCOPE();
        LOG_INFO("World - Reclaim Idle Renderer");
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
        // CreateWorldContext sets OwningContext before InitializeWorld, so keep that ordering.
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

    void CWorld::OnRelationshipComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        Registry.GetSignals<FRelationshipComponent>().OnDestroy.Disconnect<&CWorld::OnRelationshipComponentDestroyed>(this);
        ECS::Utils::RemoveFromParent(Registry, Entity);

        TVector<ECS::FEntity> SubTree;
    
        auto CollectRecursive = [&](auto& Self, ECS::FEntity Current) -> void
        {
            ECS::Utils::ForEachChild(Registry, Current, [&](ECS::FEntity Child)
            {
                Self(Self, Child);
                SubTree.push_back(Child);
            });
        };
    
        CollectRecursive(CollectRecursive, Entity);

        for (int32 i = (int32)SubTree.size() - 1; i >= 0; i--)
        {
            if (Registry.IsValid(SubTree[i]))
            {
                Registry.Destroy(SubTree[i]);
            }
        }
        
        Registry.GetSignals<FRelationshipComponent>().OnDestroy.Connect<&CWorld::OnRelationshipComponentDestroyed>(this);
    }

    void CWorld::OnRelationshipComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        // Only ever CLEARS the bit, since the reverse transition means the entity is being torn down.
        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            Transform->bIsFlat = false;
        }
    }

    void CWorld::OnTransformComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& TransformComponent = Registry.Get<STransformComponent>(Entity);
        TransformComponent.Registry = &EntityRegistry;
        TransformComponent.Entity = Entity;
        TransformComponent.DirtyState = ECS::Utils::EnsureTransformDirtyGate(EntityRegistry);

        // Checked rather than assumed, since nothing orders the two components.
        TransformComponent.bIsFlat = ECS::Utils::IsEntityTransformFlat(EntityRegistry, Entity);

        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
    }

    void CWorld::OnWidgetComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        RmlUi::ReleaseWidget(this, Registry.Get<SWidgetComponent>(Entity));
    }

    void CWorld::OnCSharpScriptComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        // Routed through the driver, since OnDetach is user code and may attach or detach scripts.
        EntityScripts::DetachAll(Registry, Entity);
    }

    CEntityScript* CWorld::AddEntityScript(ECS::FEntity Entity, FStringView ScriptClass)
    {
        if (!EntityRegistry.IsValid(Entity) || ScriptClass.empty())
        {
            return nullptr;
        }

        // Class-keyed, so this attaches a C++ script and a C# one through exactly the same call.
        CClass* Class = FindObject<CClass>(FName(ScriptClass));
        return EntityScripts::Attach(EntityRegistry, Entity, Class);
    }

    void CWorld::SetEntityScript(ECS::FEntity Entity, FStringView ScriptClass)
    {
        AddEntityScript(Entity, ScriptClass);
    }

    namespace
    {
        // List-scheduling assigns each system to the lowest-indexed batch it does not conflict with.
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

                // A C# system with no declared access stays exclusive, which is the safe default.
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
            Algo::Sort(SystemUpdateList[i].begin(), SystemUpdateList[i].end(),
                [](const FStageSlot& A, const FStageSlot& B) { return A.StagePriority < B.StagePriority; });
        }

        // A pure function of the final stage lists, so computed once instead of every stage.
        for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
        {
            SystemBatches[i] = ComputeSystemBatches(SystemUpdateList[i]);
        }

        StartupManagedSystems();
    }

    void CWorld::StartupManagedSystems()
    {
        if (!bSystemsStarted)
        {
            return;
        }

        for (FManagedSystem& Managed : ManagedSystems)
        {
            if (Managed.Instance != nullptr && !Managed.bStarted)
            {
                Managed.bStarted = true;
                DotNet::StartupManagedSystem(Managed.Instance, &SystemContext);
            }
        }
    }

    // Read-only snapshot of the per-stage batch layout for the Gameplay Insights tool.
    void CWorld::GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const
    {
        Out.clear();
        for (uint8 s = 0; s < (uint8)EUpdateStage::Max; ++s)
        {
            const TVector<FStageSlot>& Systems = SystemUpdateList[s];
            const TVector<TVector<uint16>>& Batches = SystemBatches[s];
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

        // A stale-generation instance must be DROPPED, not destroyed, since its handle was freed.
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

        // Persisted immediately; the system-list rebuild defers to ApplyPendingSystemChanges.
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

    FImmediateLineRenderer* CWorld::GetImmediateLines() const
    {
        if (IsSuspended() || RenderScene == nullptr)
        {
            return nullptr;
        }

        return RenderScene->GetImmediateLines();
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
            return NullOpt;
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
    
    void CWorld::CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) const
    {
        LUMINA_PROFILE_SCOPE();

        if (PhysicsScene == nullptr)
        {
            OutHits.clear();
            return;
        }

        PhysicsScene->CastSphere(Settings, OutHits);
    }

    TOptional<SRayResult> CWorld::CastSphereClosest(const SSphereCastSettings& Settings) const
    {
        LUMINA_PROFILE_SCOPE();

        if (PhysicsScene == nullptr)
        {
            return NullOpt;
        }

        return PhysicsScene->CastSphereClosest(Settings);
    }

    EUpdateStage CWorld::GetUpdateStage() const
    {
        return SystemContext.GetUpdateStage();
    }

    ECS::FEntity CWorld::GetEntityByTag(const FName& Tag)
    {
        const ECS::FSparseSet* Storage =
            EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag);

        if (Storage == nullptr || Storage->IsEmpty())
        {
            return ECS::NullEntity;
        }

        return *Storage->begin();
    }

    // The resolved view, not the camera component, so shake and blends are already folded in.
    const FResolvedSceneView* CWorld::GetResolvedView() const
    {
        const FResolvedSceneView* View = EntityRegistry.Ctx().Find<FResolvedSceneView>();
        return (View != nullptr && View->bHasView) ? View : nullptr;
    }

    FVector2 CWorld::GetViewportSize() const
    {
        if (RenderScene == nullptr)
        {
            return FVector2(0.0f);
        }
        const FUIntVector2 Extent = RenderScene->GetRenderExtent();
        return FVector2((float)Extent.x, (float)Extent.y);
    }

    FScreenProjection CWorld::WorldToScreen(FVector3 WorldLocation) const
    {
        FScreenProjection Result;
        const FResolvedSceneView* View = GetResolvedView();
        const FVector2 Viewport = GetViewportSize();
        if (View == nullptr || Viewport.x <= 0.0f || Viewport.y <= 0.0f)
        {
            return Result;
        }

        Result.bOnScreen = View->ViewVolume.WorldToScreen(WorldLocation, Viewport, Result.Position, Result.Depth);
        return Result;
    }

    FWorldRay CWorld::ScreenToWorldRay(FVector2 ScreenPosition) const
    {
        FWorldRay Result;
        const FResolvedSceneView* View = GetResolvedView();
        const FVector2 Viewport = GetViewportSize();
        if (View == nullptr || Viewport.x <= 0.0f || Viewport.y <= 0.0f)
        {
            return Result;
        }

        View->ViewVolume.ScreenToWorldRay(ScreenPosition, Viewport, Result.Origin, Result.Direction);
        Result.bValid = true;
        return Result;
    }

    FWorldRay CWorld::ViewportCenterRay() const
    {
        const FVector2 Viewport = GetViewportSize();
        return ScreenToWorldRay(FVector2(Viewport.x * 0.5f, Viewport.y * 0.5f));
    }

    FVector3 CWorld::DeprojectScreenToWorld(FVector2 ScreenPosition, float WorldDistance) const
    {
        const FWorldRay Ray = ScreenToWorldRay(ScreenPosition);
        return Ray.bValid ? (Ray.Origin + Ray.Direction * WorldDistance) : FVector3(0.0f);
    }

    void CWorld::GetEntitiesByTag(const FName& Tag, TVector<ECS::FEntity>& Out)
    {
        // Iterating the storage yields components; the sparse-set base is what yields the entities.
        const ECS::FSparseSet* TagStorage =
            EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag);

        if (TagStorage == nullptr)
        {
            return;
        }

        Out.reserve(Out.size() + TagStorage->Num());
        for (const ECS::FEntity Entity : *TagStorage)
        {
            if (!Entity.IsTombstone())
            {
                Out.push_back(Entity);
            }
        }
    }

    ECS::FEntity CWorld::GetEntityByName(const FName& Name)
    {
        auto View = EntityRegistry.View<SNameComponent>();
        for (ECS::FEntity Entity : View)
        {
            SNameComponent& NameComponent = View.Get<SNameComponent>(Entity);
            if (NameComponent.Name == Name)
            {
                return Entity;
            }
        }
        
        return ECS::NullEntity;
    }

    FName CWorld::GetEntityName(ECS::FEntity Entity)
    {
        const SNameComponent* Name = EntityRegistry.TryGet<SNameComponent>(Entity);
        return Name ? Name->Name : FName();
    }

    ECS::FEntity CWorld::GetFirstEntityWith(uint32 Type)
    {
        if (!EntityRegistry.FindStorage(Type))
        {
            return ECS::NullEntity;
        }

        const ECS::FSparseSet* storage = EntityRegistry.FindStorage(static_cast<ECS::FComponentTypeID>(Type));

        if (storage == nullptr || storage->IsEmpty())
        {
            return ECS::NullEntity;
        }
        return *storage->begin();
    }

    void CWorld::SetEntityTransform(ECS::FEntity Entity, const FTransform& NewTransform)
    {
        EntityRegistry.EmplaceOrReplace<STransformComponent>(Entity, NewTransform);
        EntityRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
    }

    void CWorld::TickSystems(FSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        TVector<FStageSlot>& Systems = SystemUpdateList[(uint32)Context.GetUpdateStage()];
        
        auto RunOne = [&](FStageSlot& S)
        {
            SetExecutingSystemAccess(&S.Access);
            S.Update(S.Self, Context);
            SetExecutingSystemAccess(nullptr);
        };
        
        const TVector<TVector<uint16>>& Batches = SystemBatches[(uint32)Context.GetUpdateStage()];
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
                // Create every declared pool before going wide, since view() writes the shared pool map.
                for (uint16 Index : Batch)
                {
                    for (void (*Assure)(ECS::FRegistry&) : Systems[Index].Access.PoolAssurers)
                    {
                        Assure(EntityRegistry);
                    }
                }

                Task::ParallelFor(static_cast<uint32>(Batch.size()), [&](uint32 Index)
                {
                    DEBUG_ASSERT(!Systems[Batch[Index]].Access.bExclusive); // batched implies not exclusive
                    RunOne(Systems[Batch[Index]]);
                }, 1);
            }
        }
    }
}
