#include "RuntimePCH.h"
#include "World.h"
#include "World/ECS/Registry.h"
#include "World/ECS/ECSReferenceProvider.h"
#include "Networking/NetRealm.h"
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
#include "SaveGame/WorldSaveState.h"
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
#include "Core/Object/ScriptClass.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Net/NetRole.h"
#include "Networking/INetworkRuntime.h"
#include "Subsystems/WorldSettings.h"
#include "Subsystems/CoroutineManager.h"
#include "Subsystems/TimerManager.h"
#include "Subsystems/TweenManager.h"
#include "UI/RmlUiBridge.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/BillboardComponent.h"
#include "World/Entity/Components/CloudComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/LocalFogVolumeComponent.h"
#include "World/Entity/Components/ReflectionProbeComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/SplineComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "World/Entity/Components/WaterComponent.h"

namespace Lumina
{
    namespace WorldInternal
    {
        // Every type the renderer extracts outside the mesh and transform channels, so each keeps exact change ticks.
        void TrackRenderedComponentChanges(ECS::FRegistry& Registry)
        {
            Registry.TrackChanges<SDirectionalLightComponent>();
            Registry.TrackChanges<SPointLightComponent>();
            Registry.TrackChanges<SSpotLightComponent>();
            Registry.TrackChanges<SAreaLightComponent>();
            Registry.TrackChanges<SSkyLightComponent>();
            Registry.TrackChanges<SDecalComponent>();
            Registry.TrackChanges<SSplineComponent>();
            Registry.TrackChanges<SReflectionProbeComponent>();
            Registry.TrackChanges<STextComponent>();
            Registry.TrackChanges<SSprite3DComponent>();
            Registry.TrackChanges<SAnimatedSprite3DComponent>();
            Registry.TrackChanges<SBillboardComponent>();
            Registry.TrackChanges<SWidgetComponent>();
            Registry.TrackChanges<SWaterComponent>();
            Registry.TrackChanges<SExponentialHeightFogComponent>();
            Registry.TrackChanges<SLocalFogVolumeComponent>();
            Registry.TrackChanges<SCloudComponent>();
            Registry.TrackChanges<SEnvironmentComponent>();
            Registry.TrackChanges<STerrainComponent>();
            Registry.TrackChanges<SParticleSystemComponent>();
            Registry.TrackChanges<SPostProcessComponent>();
        }
    }

    // Generous for the handful of paint ops a frame issues, and drained every Extract.
    static constexpr uint32 kRenderTargetPaintQueueCapacity = 1024;

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
    }

    void FWorldDebugInterface::DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            World->DrawBillboard(ResourceID, Location, Scale);
        }
            #endif
    }

    void FWorldDebugInterface::DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World != nullptr && !World->IsSuspended())
        {
            World->LineBatcherComponent->EnqueueLine(Start, End, Color, Thickness, bDepthTest, Duration);
        }
            #endif
    }

    void FWorldDebugInterface::DrawText(FStringView Text, TOptional<FVector4> Color)
    {
        #if !defined(LE_SHIPPING)
        if (World != nullptr && !World->IsSuspended())
        {
            FDebugTextLine& Line = World->DebugTextLines.EmplaceGetRef();
            Line.Text = FString(Text);
            Line.Color = Color.Get(FVector4(1.0f));
        }
        #endif
    }
    void FWorldDebugInterface::DrawLine(FVector3 Start, FVector3 End, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            DrawLine(Start, End, Color, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }
    void FWorldDebugInterface::DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            IPrimitiveDrawInterface::DrawBox(Center, HalfExtents, Rotation, Color, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }
    void FWorldDebugInterface::DrawSphere(FVector3 Center, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            IPrimitiveDrawInterface::DrawSphere(Center, Radius, Color, 16, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }
    void FWorldDebugInterface::DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            IPrimitiveDrawInterface::DrawCapsule(Start, End, Radius, Color, 16, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }
    void FWorldDebugInterface::DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            IPrimitiveDrawInterface::DrawCone(Apex, Direction, AngleRadians, Length, Color, 16, 4, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }
    void FWorldDebugInterface::DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World)
        {
            IPrimitiveDrawInterface::DrawArrow(Start, Direction, Length, Color, Thickness.Get(1.0f), bDepthTest.Get(true), Duration.Get(0.0f));
        }
            #endif
    }

    void FWorldDebugInterface::DrawSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (World != nullptr && !World->IsSuspended())
        {
            World->TriangleBatcherComponent->EnqueueTriangles(Move(Vertices), Mode, Duration);
        }
            #endif
    }

    CWorld::CWorld()
        : CommandBus(EntityRegistry)
        , EventQueues(EntityRegistry)
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

    void CWorld::PaintRenderTarget(CTextureRenderTarget* Target, FVector2 UV, float RadiusUV, FVector4 Color, float Strength, float Hardness, CTexture* BrushMask)
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

    void CWorld::ClearRenderTarget(CTextureRenderTarget* Target, FVector4 Color)
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
            OutOps.Add(Move(Op));
        }
    }

    void CWorld::Serialize(FArchive& Ar)
    {
        LUMINA_PROFILE_SCOPE();
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
            ECS::FRegistry& Source = (EntityRegistry.NumEntities() != 0) ? EntityRegistry : RegistryPending;
            ECS::Utils::SerializeRegistry(Ar, Source);
        }
    }

    void CWorld::VisitAdditionalObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        VisitRegistryObjectReferences(EntityRegistry, Func);
        VisitRegistryObjectReferences(RegistryPending, Func);
    }

    void CWorld::PreLoad()
    {
    }

    void CWorld::PostLoad()
    {
    }
    
    void CWorld::InitializeWorld(EWorldType InWorldType)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("World");
        WorldType = InWorldType;
        
        {
            LUMINA_PROFILE_SECTION("CullOrphanedPrefabs");
            CPrefab::CullOrphanedInstances(RegistryPending);
        }
        
        if (RegistryPending.NumEntities() != 0)
        {
            // Pending commands and events name entities of the registry being swapped out.
            CommandBus.Discard();
            EventQueues.Clear();
            EntityRegistry.Swap(RegistryPending);
        }
        RegistryPending = {};

        {
            LUMINA_PROFILE_SECTION("RefreshPrefabInstances");
            CPrefab::RefreshAllInstancesInWorld(this);
        }

        // A tag serializes as its component, but FindByTag reads the per-tag storage, so refill it here.
        ECS::Utils::RebuildTagStorages(EntityRegistry);

        {
            LUMINA_PROFILE_SECTION("CompactRegistry");
            EntityRegistry.Compact();
        }

        // Before any system or script spawns, so a save can tell what the level made from what play made.
        SaveGame::RecordLevelEntities(EntityRegistry);
        
        // Which entities a client may hold is a netcode question, so it is reported not decided.
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->OnWorldEntitiesLoaded(this);
        }

        EntityRegistry.Ctx().Emplace<ECS::FEventQueues*>(&EventQueues);

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
            LUMINA_PROFILE_SECTION("CreatePhysicsScene");
            PhysicsScene = Physics::GetPhysicsContext()->CreatePhysicsScene(this);
            InstallPhysicsScriptHook();
        }
        // Emplaced even when null so ctx().get consumers find the key and null-check it.
        EntityRegistry.Ctx().Emplace<Physics::IPhysicsScene*>(PhysicsScene.get());
        EntityRegistry.Ctx().Emplace<SSystemContext*>(&SystemContext);
        EntityRegistry.Ctx().Emplace<CWorld*>(this);

        // A world entity owns its subtree, so destroying one takes its descendants along.
        EntityRegistry.SetDestroyDescendantsWithParent(true);

        // CCameraSystem owns FCameraGlobalState and writes FResolvedSceneView for Extract.
        EntityRegistry.Ctx().Emplace<FCameraGlobalState>();
        EntityRegistry.Ctx().Emplace<FResolvedSceneView>();
        EntityRegistry.Ctx().Emplace<FWorldSettingsState>();
        EntityRegistry.Ctx().GetOrEmplace<FTimerManager>();
        EntityRegistry.Ctx().GetOrEmplace<FCoroutineManager>();
        EntityRegistry.Ctx().GetOrEmplace<FTweenManager>().SetWorldRegistry(&EntityRegistry);
        SyncWorldSettingsState();

        {
            LUMINA_PROFILE_SECTION("CreateRenderer");
            CreateRenderer();
        }
        UIContext = RmlUi::CreateWorldUI(this);

        // Seeded before registering systems so a disabled system is never constructed.
        DisabledSystems.Reset();
        for (const FName& Name : GetDefaultWorldSettings().DisabledSystems)
        {
            DisabledSystems.Add(Name);
        }
        PendingDisabledSystems = DisabledSystems;

        {
            LUMINA_PROFILE_SECTION("RegisterSystems");
            RegisterSystems();
        }
        
        if (WorldType == EWorldType::Game || WorldType == EWorldType::Simulation)
        {
            LUMINA_PROFILE_SECTION("PhysicsSimulate");
            PhysicsScene->Simulate();
        }
        
        bSystemsStarted = true;
        {
            LUMINA_PROFILE_SECTION("StartupSystems");
            StartupPendingSystems();
        }

        EntityRegistry.GetSignals<STransformComponent>().OnConstruct    .Connect<&ThisClass::OnTransformComponentConstruct>(this);
        EntityRegistry.GetSignals<SDecalComponent>().OnConstruct        .Connect<&ThisClass::OnDecalComponentConstruct>(this);

        // Loaded, duplicated and startup-spawned decals arrived before the hook, and SpawnTime is not serialized.
        for (ECS::FEntity Decal : EntityRegistry.View<SDecalComponent>())
        {
            OnDecalComponentConstruct(EntityRegistry, Decal);
        }
        EntityRegistry.GetSignals<SEntityScriptComponent>().OnDestroy   .Connect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityRegistry.OnEntityDestroyed()                              .Connect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityRegistry.GetSignals<SWidgetComponent>().OnDestroy         .Connect<&ThisClass::OnWidgetComponentDestroyed>(this);
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnConstruct   .Connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnUpdate      .Connect<&MarkMeshResolveDirty<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SStaticMeshComponent>().OnDestroy     .Connect<&MarkMeshRemoved<SStaticMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnConstruct  .Connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnUpdate     .Connect<&MarkMeshResolveDirty<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SDynamicMeshComponent>().OnDestroy    .Connect<&MarkMeshRemoved<SDynamicMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnConstruct .Connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnUpdate    .Connect<&MarkMeshResolveDirty<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SSkeletalMeshComponent>().OnDestroy   .Connect<&MarkMeshRemoved<SSkeletalMeshComponent>>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnConstruct      .Connect<&MarkFoliageResolveDirty>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnUpdate         .Connect<&MarkFoliageResolveDirty>();
        EntityRegistry.GetSignals<SFoliageComponent>().OnDestroy        .Connect<&MarkFoliageRemoved>();
        EntityRegistry.GetSignals<SDisabledTag>().OnConstruct           .Connect<&MarkRenderVisibilityDirty>();
        EntityRegistry.GetSignals<SDisabledTag>().OnDestroy             .Connect<&MarkRenderVisibilityDirty>();

        // A queue swapped in with the loaded registry predates this world's hooks, so it sweeps once; a new one starts with a sweep anyway.
        if (FMeshResolveQueue* ResolveQueue = FMeshResolveQueue::Find(EntityRegistry))
        {
            ResolveQueue->RequestFullSweep();
        }
        FRenderDirtyTracker::Ensure(EntityRegistry).RequestFullRescan();
        WorldInternal::TrackRenderedComponentChanges(EntityRegistry);

        ECS::Utils::FTransformDirtyGate* DirtyState = ECS::Utils::EnsureTransformDirtyGate(EntityRegistry);
        auto TransformView = EntityRegistry.View<STransformComponent>();
        TransformView.ForEach([&](ECS::FEntity Entity, STransformComponent& TransformComponent)
        {
            TransformComponent.Registry = &EntityRegistry;
            TransformComponent.Entity = Entity;
            TransformComponent.DirtyState = DirtyState;
            TransformComponent.bIsFlat = ECS::Utils::IsEntityTransformFlat(EntityRegistry, Entity);
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

        auto CameraView = EntityRegistry.View<SCameraComponent, ECS::Without<SDisabledTag>>();
        CameraView.ForEach([&](ECS::FEntity Entity, const SCameraComponent& Camera)
        {
           if (Camera.bAutoActivate)
           {
               GetCamera()->SetActiveCamera(Entity);
           }
        });

        if (WorldType == EWorldType::Simulation || WorldType == EWorldType::Game)
        {
            bPaused = false;
        }
        
        // Last, so a subsystem's OnInitialize sees a world whose registry, systems and renderer are up.
        WorldSubsystems::CreateMissing(*this, Subsystems);
        RefreshEngineSubsystems();

        bInitializing = false;
    }
    
    void CWorld::TeardownWorld()
    {
        bTearingDown = true;

        // First, so every subscription the managed side drops still disconnects against a live world.
        DotNet::NotifyWorldTeardown(this);

        // Scripts go before anything they can reach, so every OnDetach still sees live subsystems, physics and timers.
        EntityRegistry.GetSignals<SEntityScriptComponent>().OnDestroy.Disconnect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityRegistry.OnEntityDestroyed().Disconnect<&ThisClass::OnCSharpScriptComponentDestroyed>(this);
        EntityScripts::DetachAllInRegistry(EntityRegistry);

        // Before the systems and the registry go, so a subsystem can still read the world it leaves.
        WorldSubsystems::DestroyAll(Subsystems);
        EngineSubsystems = {};

        // No render phase / RHI / audio device in a headless process.
        if (!GIsHeadless)
        {
            RHI::WaitDeviceIdle();
        }

        // The viewport outlives the world, so an unpopped layer would gate input in the next PIE.
        Input::ClearLayers(this);

        Audio::Context().StopAllSounds();

        EntitySystems::DestroyAll(SystemContext, Systems);
        for (int32 i = 0; i < (int32)EUpdateStage::Max; ++i)
        {
            StageSchedules[i] = {};
            SystemSegments[i].Reset();
            SystemsWithWork[i].Reset();
        }

        // Keyed on the scene, since StopSimulate is what disconnects its registry listeners.
        if (PhysicsScene != nullptr)
        {
            PhysicsScene->StopSimulate();
        }

        RegistryPending.Clear();
        EntityRegistry.Clear();

        // After the registry, since clearing it runs OnDetach on the context this destroys.
        RmlUi::DestroyWorldUI(this);
        UIContext.reset();

        PhysicsScene.reset();
        DestroyRenderer();

        FCoreDelegates::Get().PostWorldUnload.Broadcast();
    }

    void CWorld::Update(const FUpdateContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        const EUpdateStage Stage = Context.GetUpdateStage();

        // Applied between frames, so a mid-frame toggle never lands inside a running batch.
        ApplyPendingSystemChanges();

        // Rebuilt before any tick, so a retired script class is never the target of a virtual call.
        if (Stage == EUpdateStage::FrameStart && DotNet::IsInitialized()
            && DotNet::GetScriptGeneration() != ScriptGeneration)
        {
            EntitySystems::DropScripted(SystemContext, Systems);
            RegisterSystems();

            // A reload retires the old script classes, so the scripted subsystems rebuild against the new ones.
            WorldSubsystems::DropScripted(Subsystems);
            WorldSubsystems::CreateMissing(*this, Subsystems);
            RefreshEngineSubsystems();
        }

        // Joining a host turns a standalone world into a client, which changes what HostOnly and ClientOnly allow.
        if (Stage == EUpdateStage::FrameStart && NetRealm::KeyOf(this) != RegisteredRealmKey)
        {
            EntitySystems::DropOutsideRealm(SystemContext, *this, Systems);
            RegisterSystems();
            WorldSubsystems::DropOutsideRealm(*this, Subsystems);
            WorldSubsystems::CreateMissing(*this, Subsystems);
            RefreshEngineSubsystems();
        }

        if (Stage == EUpdateStage::FrameStart)
        {
            // Before the pause check, so what was sent last frame still ages out while the game is paused.
            EventQueues.Advance();

            RealDeltaTime = Context.GetDeltaTime();
            DeltaTime = RealDeltaTime * GetDefaultWorldSettings().DeltaTimeScale;
            TimeSinceCreation += DeltaTime;
        }

        if ((bPaused && Stage != EUpdateStage::Paused) || (!bPaused && Stage == EUpdateStage::Paused))
        {
            return;
        }

        // Ahead of the stage lists, so a system reads state a subsystem already settled this frame.
        if (Stage == EUpdateStage::FrameStart)
        {
            WorldSubsystems::Update(Subsystems, (float)DeltaTime);
        }

        // Every stage, since the editor and scripts change the settings in place between any two.
        SyncWorldSettingsState();

        SystemContext.DeltaTime     = DeltaTime;
        SystemContext.RealDeltaTime = RealDeltaTime;
        SystemContext.Time          = TimeSinceCreation;
        SystemContext.UpdateStage   = Stage;

        // Deferred timers run inside TickSystems now, still before gameplay systems.
        TickSystems(SystemContext);

        // After the variable-rate PrePhysics systems and before the physics step.
        if (Stage == EUpdateStage::PrePhysics)
        {
            TickFixedUpdate(DeltaTime);
        }

        // The frame's one propagation, owned here so a world with no renderer still settles, since physics syncs bodies from local transforms.
        if ((Stage == EUpdateStage::FrameEnd || Stage == EUpdateStage::Paused) && ECS::Utils::AnyTransformsDirty(EntityRegistry))
        {
            ECS::Utils::ResolveAllDirtyTransforms(EntityRegistry);
        }
    }

    Physics::IPhysicsScene* CWorld::EnsurePhysicsScene()
    {
        if (PhysicsScene == nullptr)
        {
            PhysicsScene = Physics::GetPhysicsContext()->CreatePhysicsScene(this);

            // Simulate() connects the hook that turns SRigidBodyComponent into an actual body.
            PhysicsScene->Simulate();
            InstallPhysicsScriptHook();
        }

        return PhysicsScene.get();
    }

    void CWorld::InstallPhysicsScriptHook()
    {
        PhysicsScene->SetPreStepCallback([this](float FixedDeltaTime)
        {
            EntityScripts::TickFixed(EntityRegistry, FixedDeltaTime);
        });
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
            RenderScene->Extract(View.ViewVolume, View.bHasPostProcess ? &View.PostProcess : nullptr, View.Layers);
            return;
        }

        RenderScene->SetActivePostProcessMaterials({});
        RenderScene->Extract(FViewVolume{}, nullptr, FViewLayerSettings{});
    }

    ECS::FEntity CWorld::ConstructEntity(FName Name, const FTransform& Transform)
    {
        LUMINA_PROFILE_SCOPE();
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

    bool CWorld::FractureEntity(ECS::FEntity Entity, FVector3 Origin, float Strength)
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
            if (EntityRegistry.TryGet<SRigidBodyComponent>(Entity) != nullptr)
            {
                if (PhysicsScene->GetBodyStatus(Entity) == Physics::EPhysicsBodyStatus::Ready)
                {
                    OwnerTransform.SetLocation(PhysicsScene->GetBodyPosition(Entity));
                    OwnerTransform.SetRotation(PhysicsScene->GetBodyRotation(Entity));
                    InheritedVelocity       = PhysicsScene->GetLinearVelocity(Entity);
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
        auto LaunchBody = [&](ECS::FEntity Fragment, const FVector3& WorldCenter, uint32 Seed)
        {
            if (!PhysicsScene)
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
            PhysicsScene->SetLinearVelocity(Fragment, LaunchVelocity);

            if (SpinSpeed > 0.0f)
            {
                const FVector3 Spin(Hash01(Seed + 4) - 0.5f, Hash01(Seed + 5) - 0.5f, Hash01(Seed + 6) - 0.5f);
                PhysicsScene->SetAngularVelocity(Fragment, Spin * (2.0f * SpinSpeed));
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

        // Cap fragments at physics body headroom, since overflowing the body arrays trips a hard assert.
        uint32 MaxFragments = 0xFFFFFFFFu;
        if (PhysicsScene)
        {
            const uint32 MaxBodies = PhysicsScene->GetMaxBodyCount();
            const uint32 Used      = Math::Min(PhysicsScene->GetBodyCount(), MaxBodies);
            const uint32 Headroom  = MaxBodies - Used;
            MaxFragments = Headroom > 16 ? Headroom - 16 : 0;

            const uint32 Desired = Pieces.IsEmpty()
                ? (uint32)Math::Clamp(Destructible->FragmentCount, 2, 512)
                : (uint32)Pieces.Num();
            if (Desired > MaxFragments)
            {
                LOG_WARN("FractureEntity: clamped {} fragments to {} (physics body headroom {}/{}). Raise World Settings > Physics > MaxPhysicsBodies.",
                    Desired, MaxFragments, Used, MaxBodies);
            }
        }

        if (!Pieces.IsEmpty())
        {
            const TVector<TStrongObjectPtr<CMaterialInterface>>& PieceMaterials =
                (CollectionData && !Destructible->Collection->Materials.IsEmpty())
                    ? Destructible->Collection->Materials
                    : SourceMesh->Materials;

            // Pre-baked collections cache piece meshes; the Voronoi path builds each one inline.
            const TVector<TStrongObjectPtr<CStaticMesh>>* CachedMeshes =
                CollectionData ? &Destructible->Collection->GetPieceMeshes() : nullptr;

            for (int32 PieceIndex = 0; PieceIndex < Pieces.Num() && (uint32)Spawned < MaxFragments; ++PieceIndex)
            {
                const FFracturePiece& Piece = Pieces[PieceIndex];

                CStaticMesh* PieceMesh = CachedMeshes
                    ? (PieceIndex < CachedMeshes->Num() ? (*CachedMeshes)[PieceIndex].Get() : nullptr)
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

                SMeshColliderComponent ColliderDesc;
                ColliderDesc.Mesh    = PieceMesh;
                ColliderDesc.bConvex = true;
                EntityRegistry.Emplace<SMeshColliderComponent>(Fragment, Move(ColliderDesc));

                EntityRegistry.Emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.Emplace<SFragmentComponent>(Fragment).Source   = (Entity).Value;
                
                LaunchBody(Fragment, WorldCenter, (Fragment).Value + static_cast<uint32>(Spawned));

                ++Spawned;
            }
        }
        else
        {
            // Degenerate fracture falls back to subdividing the bounds into textured box chunks.
            const SAABB& LocalBounds = SourceMesh->GetAABB();
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

                SBoxColliderComponent BoxDesc;
                BoxDesc.HalfExtent = ColliderHalf;
                EntityRegistry.Emplace<SBoxColliderComponent>(Fragment, Move(BoxDesc));

                EntityRegistry.Emplace<SLifetimeComponent>(Fragment).Lifetime = Destructible->FragmentLifetime;
                EntityRegistry.Emplace<SFragmentComponent>(Fragment).Source   = (Entity).Value;

                LaunchBody(Fragment, CellWorldCenter, (Fragment).Value + static_cast<uint32>(Spawned));

                ++Spawned;
            }
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

    void CWorld::SetEntityLifetime(ECS::FEntity Entity, float Seconds)
    {
        if (Seconds > 0.0f && IsValidEntity(Entity))
        {
            GetOrEmplaceComponent<SLifetimeComponent>(Entity).Lifetime = Seconds;
        }
    }

    void CWorld::DuplicateEntity(ECS::FEntity& To, ECS::FEntity From, const TFunctionRef<bool(const ECS::FComponentTypeInfo&)>& Callback)
    {
        LUMINA_MEMORY_SCOPE("World");
        ASSERT(To != From);

        THashMap<ECS::FEntity, ECS::FEntity> SourceToDuplicate;

        auto DuplicateRecursive = [&](this auto const& Self, ECS::FEntity Source, ECS::FEntity NewParent) -> ECS::FEntity
        {
            ECS::FEntity NewEntity = EntityRegistry.Create();
            SourceToDuplicate[Source] = NewEntity;

            TVector<Lumina::ECS::FSparseSet*> CopiedStorages;

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
                if (ID == ECS::GetComponentTypeID<SRigidBodyComponent>())
                {
                    continue;
                }

                if (Storage.Contains(Source) && !Storage.Contains(NewEntity))
                {
                    Storage.EmplaceCopyRaw(NewEntity, Storage.GetRaw(Source));
                    CopiedStorages.Add(&Storage);
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

                EntityRegistry.Remove<SRigidBodyComponent>(NewEntity);
                EntityRegistry.Emplace<SRigidBodyComponent>(NewEntity, Move(NewBody));
            }

            // The raw copy writes straight into the pool, so nothing observing on_construct saw the duplicate.
            for (Lumina::ECS::FSparseSet* Copied : CopiedStorages)
            {
                Copied->Signals.OnConstruct.Broadcast(EntityRegistry, NewEntity);
            }

            if (NewParent != ECS::NullEntity)
            {
                ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, NewParent, false);
            }
            else if (const ECS::FEntity SourceParent = EntityRegistry.GetHierarchy().GetParent(Source); SourceParent != ECS::NullEntity)
            {
                ECS::Utils::ReparentEntity(EntityRegistry, NewEntity, SourceParent, false);
            }

            // Attaching appends, so walking the children in order keeps the copy's sibling order.
            EntityRegistry.GetHierarchy().ForEachChild(Source, [&](ECS::FEntity Child)
            {
                Self(Child, NewEntity);
            });

            return NewEntity;
        };

        To = DuplicateRecursive(From, ECS::NullEntity);

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
        return EntityRegistry.GetHierarchy().GetParent(Entity);
    }

    ECS::FEntity CWorld::GetRootEntity(ECS::FEntity Entity)
    {
        return EntityRegistry.GetHierarchy().GetRoot(Entity);
    }

    ECS::FEntity CWorld::GetFirstChild(ECS::FEntity Entity) const
    {
        return EntityRegistry.GetHierarchy().GetFirstChild(Entity);
    }

    ECS::FEntity CWorld::GetNextSibling(ECS::FEntity Entity) const
    {
        return EntityRegistry.GetHierarchy().GetNextSibling(Entity);
    }

    void CWorld::GetAncestorChain(ECS::FEntity Entity, TVector<ECS::FEntity>& Out) const
    {
        const ECS::FHierarchy& Hierarchy = EntityRegistry.GetHierarchy();
        for (ECS::FEntity Current = Entity; Current != ECS::NullEntity; Current = Hierarchy.GetParent(Current))
        {
            Out.Add(Current);
        }
    }

    void CWorld::GetSubtree(ECS::FEntity Entity, TVector<ECS::FEntity>& Out) const
    {
        Out.Add(Entity);
        EntityRegistry.GetHierarchy().ForEachDescendant(Entity, [&Out](ECS::FEntity Descendant) { Out.Add(Descendant); });
    }

    void CWorld::DestroyEntity(ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        EntityScripts::CheckParallelStructureChange("DestroyEntity");

        // A parallel system or worker would destroy under other readers, so it waits for the sync point.
        if (ECS::FCommandBus::ShouldDefer())
        {
            CommandBus.Destroy(Entity);
            return;
        }
        EntityRegistry.Destroy(Entity);
    }

    FTransform CWorld::GetEntityTransform(ECS::FEntity Entity)
    {
        const STransformComponent* Transform = FindScriptTransform(Entity, "GetEntityTransform");
        return Transform != nullptr ? Transform->GetWorldTransform() : FTransform();
    }

    // Scripts reach these by handle and can race teardown, so a missing transform is a no-op rather than a fatal assert.
    STransformComponent* CWorld::FindScriptTransform(ECS::FEntity Entity, const char* Caller)
    {
        EntityScripts::CheckParallelAccess(Entity, Caller);
        STransformComponent* Transform = EntityRegistry.IsValid(Entity) ? EntityRegistry.TryGet<STransformComponent>(Entity) : nullptr;
        if (Transform == nullptr)
        {
            LOG_WARN("CWorld::{} on entity {} with no transform, which was likely destroyed already.", Caller, Entity.GetPacked());
        }
        return Transform;
    }

    FVector3 CWorld::GetEntityLocation(ECS::FEntity Entity)
    {
        const STransformComponent* Transform = FindScriptTransform(Entity, "GetEntityLocation");
        return Transform != nullptr ? Transform->GetWorldLocation() : FVector3(0.0f);
    }

    FQuat CWorld::GetEntityRotation(ECS::FEntity Entity)
    {
        const STransformComponent* Transform = FindScriptTransform(Entity, "GetEntityRotation");
        return Transform != nullptr ? Transform->GetWorldRotation() : FQuat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    FVector3 CWorld::GetEntityScale(ECS::FEntity Entity)
    {
        const STransformComponent* Transform = FindScriptTransform(Entity, "GetEntityScale");
        return Transform != nullptr ? Transform->GetWorldScale() : FVector3(1.0f);
    }

    void CWorld::SetEntityLocation(ECS::FEntity Entity, FVector3 Location)
    {
        // An unchanged write is skipped, since marking it dirty would re-sync the entity downstream for nothing.
        STransformComponent* Transform = FindScriptTransform(Entity, "SetEntityLocation");
        if (Transform != nullptr && Transform->GetLocalLocation() != Location)
        {
            Transform->SetLocation(Location);
        }
    }

    void CWorld::SetEntityRotation(ECS::FEntity Entity, FQuat Rotation)
    {
        STransformComponent* Transform = FindScriptTransform(Entity, "SetEntityRotation");
        if (Transform != nullptr && !(Transform->GetLocalRotation() == Rotation))
        {
            Transform->SetRotation(Rotation);
        }
    }

    void CWorld::SetEntityScale(ECS::FEntity Entity, FVector3 Scale)
    {
        STransformComponent* Transform = FindScriptTransform(Entity, "SetEntityScale");
        if (Transform != nullptr && Transform->GetLocalScale() != Scale)
        {
            Transform->SetLocalScale(Scale);
        }
    }

    void CWorld::SetEntityLocationAndRotation(ECS::FEntity Entity, FVector3 Location, FQuat Rotation)
    {
        STransformComponent* Transform = FindScriptTransform(Entity, "SetEntityLocationAndRotation");
        if (Transform != nullptr && (Transform->GetLocalLocation() != Location || !(Transform->GetLocalRotation() == Rotation)))
        {
            Transform->SetLocalLocationAndRotation(Location, Rotation);
        }
    }

    void CWorld::SetEntityTransforms(const ECS::FEntity* Entities, int32 EntityCount, const FTransform* Transforms, int32 TransformCount)
    {
        LUMINA_PROFILE_SCOPE();

        if (Entities == nullptr || Transforms == nullptr)
        {
            return;
        }

        const int32 Count = Math::Min(EntityCount, TransformCount);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (STransformComponent* Transform = EntityRegistry.TryGet<STransformComponent>(Entities[Index]))
            {
                Transform->SetLocalTransform(Transforms[Index]);
            }
        }
    }

    FVector3 CWorld::TranslateEntity(ECS::FEntity Entity, FVector3 Translation)
    {
        STransformComponent* Transform = FindScriptTransform(Entity, "TranslateEntity");
        return Transform != nullptr ? Transform->Translate(Translation) : FVector3(0.0f);
    }

    uint32 CWorld::GetNumEntities() const
    {
        return (uint32)EntityRegistry.NumEntities();
    }

    void CWorld::SetTimeDilation(float Dilation)
    {
        GetDefaultWorldSettings().DeltaTimeScale = Math::Max(Dilation, 0.0f);
    }

    float CWorld::GetTimeDilation() const
    {
        const SDefaultWorldSettings* Settings = EntityRegistry.IsValid(SingletonEntity) ? EntityRegistry.TryGet<SDefaultWorldSettings>(SingletonEntity) : nullptr;
        return Settings != nullptr ? Settings->DeltaTimeScale : 1.0f;
    }

    SWorldRenderSettings& CWorld::GetRenderSettings()
    {
        ASSERT(EntityRegistry.IsValid(SingletonEntity), "Render settings live on the singleton entity, which InitializeWorld creates.");
        return EntityRegistry.GetOrEmplace<SWorldRenderSettings>(SingletonEntity);
    }

    void CWorld::SetAnimatesWhilePaused(bool bAnimate)
    {
        bAnimatesWhilePaused = bAnimate;
        SyncWorldSettingsState();
    }

    void CWorld::SyncWorldSettingsState()
    {
        FWorldSettingsState* State = EntityRegistry.Ctx().Find<FWorldSettingsState>();
        if (State == nullptr)
        {
            return;
        }

        const SDefaultWorldSettings& Settings = GetDefaultWorldSettings();
        State->Gravity              = Settings.GravityDirection * (9.81f * Settings.GravityScale);
        State->bAnimatesWhilePaused = bAnimatesWhilePaused;
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

    bool CWorld::EntityHasTag(ECS::FEntity Entity, const FName& Tag)
    {
        if (const ECS::FSparseSet* Storage = EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag))
        {
            return Storage->Contains(Entity);
        }
        
        return false;
    }

    void CWorld::SetEntityTag(ECS::FEntity Entity, const FName& Tag)
    {
        ECS::Utils::SetEntityTag(EntityRegistry, Entity, Tag);
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

            // An idle reclaim destroys the previous scene, and terrain only uploads what it marks dirty.
            EntityRegistry.View<STerrainComponent>().ForEach([](ECS::FEntity, STerrainComponent& Terrain)
            {
                Terrain.CPUState.MarkRendererLost();
            });
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

    bool CWorld::IsBotWorld() const
    {
        return OwningContext != nullptr && OwningContext->bBot;
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

    bool CWorld::HasPresentation() const
    {
        return !GIsHeadless && GetNetMode() != ENetMode::DedicatedServer && !IsBotWorld();
    }

    bool CWorld::ShouldRender() const
    {
        // CreateWorldContext sets OwningContext before InitializeWorld, so keep that ordering.
        return !GIsHeadless && GetNetMode() != ENetMode::DedicatedServer;
    }

    CWorld* CWorld::DuplicateWorld(CWorld* OwningWorld)
    {
        LUMINA_PROFILE_SCOPE();
        CPackage* OuterPackage = OwningWorld->GetPackage();
        if (OuterPackage == nullptr)
        {
            return nullptr;
        }

        TVector<uint8> Data;
        FMemoryWriter Writer(Data);
        FInProcessObjectArchiver WriterProxy(Writer);
        {
            LUMINA_PROFILE_SECTION("Write");
            OwningWorld->Serialize(WriterProxy);
        }
        
        FMemoryReader Reader(Data);
        FInProcessObjectArchiver ReaderProxy(Reader);
        
        CWorld* PIEWorld = NewObject<CWorld>(nullptr, OwningWorld->GetName(), FGuid::New(), OF_Transient);

        PIEWorld->PreLoad();
        {
            LUMINA_PROFILE_SECTION("Read");
            PIEWorld->Serialize(ReaderProxy);
        }
        PIEWorld->PostLoad();

        return PIEWorld;
    }

    // Once per world rather than per renderer, so every view fades a timed decal on the same clock.
    void CWorld::OnDecalComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        SDecalComponent& Decal = Registry.Get<SDecalComponent>(Entity);
        if (Decal.SpawnTime < 0.0f)
        {
            Decal.SpawnTime = (float)GetTimeSinceWorldCreation();
        }
    }

    void CWorld::OnTransformComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& TransformComponent = Registry.Get<STransformComponent>(Entity);
        TransformComponent.Registry = &EntityRegistry;
        TransformComponent.Entity = Entity;
        TransformComponent.DirtyState = ECS::Utils::EnsureTransformDirtyGate(EntityRegistry);

        // A copied component's guards describe its source's queue, and a set one would keep the tag below from ever enqueuing it.
        TransformComponent.ResetDirtyState();

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

    CEntityScript* CWorld::AddEntityScript(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass)
    {
        if (!EntityRegistry.IsValid(Entity) || !ScriptClass.IsValid())
        {
            return nullptr;
        }
        return EntityScripts::Attach(EntityRegistry, Entity, ScriptClass.Get());
    }

    CEntityScript* CWorld::FindEntityScript(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass)
    {
        return ScriptClass.IsValid() ? EntityScripts::Find(EntityRegistry, Entity, ScriptClass.Get()) : nullptr;
    }

    void CWorld::FindEntityScripts(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass, TVector<TStrongObjectPtr<CEntityScript>>& Out)
    {
        if (!ScriptClass.IsValid())
        {
            return;
        }

        TVector<CEntityScript*> Found;
        EntityScripts::FindAll(EntityRegistry, Entity, ScriptClass.Get(), Found);
        Out.Reserve(Out.Num() + Found.Num());
        for (CEntityScript* Script : Found)
        {
            Out.Emplace(Script);
        }
    }

    void CWorld::FindScriptsInWorld(TSubclassOf<CEntityScript> ScriptClass, TVector<TStrongObjectPtr<CEntityScript>>& Out)
    {
        if (!ScriptClass.IsValid())
        {
            return;
        }

        for (ECS::FEntity Entity : EntityRegistry.View<SEntityScriptComponent>())
        {
            for (const TStrongObjectPtr<CEntityScript>& Script : EntityRegistry.Get<SEntityScriptComponent>(Entity).Scripts)
            {
                if (Script.Get() != nullptr && Script->IsAttached() && Script->GetClass()->IsChildOf(ScriptClass.Get()))
                {
                    Out.Add(Script);
                }
            }
        }
    }

    bool CWorld::RemoveEntityScript(ECS::FEntity Entity, CEntityScript* Script)
    {
        return Script != nullptr && EntityScripts::Remove(EntityRegistry, Entity, Script);
    }

    namespace
    {
        // A system answers to its sets, its own class and every class it derives from, so ordering against a base covers its subclasses.
        bool IsSystemNamed(const CEntitySystem* System, FName Name)
        {
            if (Algo::Contains(System->GetSets(), Name))
            {
                return true;
            }

            for (const CClass* Class = System->GetClass(); Class != nullptr && Class != CEntitySystem::StaticClass(); Class = Class->GetSuperClass())
            {
                if (Class->GetName() == Name)
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool SystemSchedule::DeclaresOrder(const CEntitySystem* First, const CEntitySystem* Second)
    {
        return Algo::AnyOf(Second->GetRunsAfter(), [First](FName Name) { return IsSystemNamed(First, Name); })
            || Algo::AnyOf(First->GetRunsBefore(), [Second](FName Name) { return IsSystemNamed(Second, Name); });
    }

    CWorld::FStageSchedule SystemSchedule::OrderStage(const TVector<CEntitySystem*>& Systems, EUpdateStage Stage)
    {
        const int32 Count = Systems.Num();

        TVector<TVector<uint16>> Predecessors(Count);
        TVector<TVector<uint16>> Successors(Count);
        TVector<uint16>          Waiting(Count, 0);
        for (uint16 First = 0; First < (uint16)Count; ++First)
        {
            for (uint16 Second = 0; Second < (uint16)Count; ++Second)
            {
                if (First != Second && DeclaresOrder(Systems[First], Systems[Second]))
                {
                    Successors[First].Add(Second);
                    Predecessors[Second].Add(First);
                    ++Waiting[Second];
                }
            }
        }

        // The longest declared chain each system starts, bounded by Count passes so a cycle still settles.
        TVector<int32> Height(Count, 1);
        for (int32 Pass = 0; Pass < Count; ++Pass)
        {
            for (uint16 Index = 0; Index < (uint16)Count; ++Index)
            {
                for (uint16 Successor : Successors[Index])
                {
                    Height[Index] = Math::Max(Height[Index], Height[Successor] + 1);
                }
            }
        }

        // The batch a system would join if placed next, one past every placed system it waits on or conflicts with.
        TVector<int32> Level(Count, 0);
        TVector<uint8> Placed(Count, 0);
        auto EarliestLevel = [&](uint16 Index)
        {
            int32 Earliest = 0;
            for (uint16 Other = 0; Other < (uint16)Count; ++Other)
            {
                if (Placed[Other] && (Algo::Contains(Predecessors[Index], Other) || FSystemAccess::Conflicts(Systems[Other]->GetAccess(), Systems[Index]->GetAccess())))
                {
                    Earliest = Math::Max(Earliest, Level[Other] + 1);
                }
            }
            return Earliest;
        };

        // List scheduling, so the earliest batch wins, then the longer chain, then shared over exclusive, then the name.
        auto Sooner = [&](uint16 A, uint16 B)
        {
            if (Level[A] != Level[B])
            {
                return Level[A] < Level[B];
            }
            if (Height[A] != Height[B])
            {
                return Height[A] > Height[B];
            }
            const bool bExclusiveA = Systems[A]->GetAccess().bExclusive;
            const bool bExclusiveB = Systems[B]->GetAccess().bExclusive;
            if (bExclusiveA != bExclusiveB)
            {
                return bExclusiveB;
            }
            return Systems[A]->GetClass()->GetName().ToString() < Systems[B]->GetClass()->GetName().ToString();
        };

        TVector<uint16> Order;
        bool            bReportedCycle = false;
        while (Order.Num() < Count)
        {
            for (uint16 Index = 0; Index < (uint16)Count; ++Index)
            {
                if (!Placed[Index])
                {
                    Level[Index] = EarliestLevel(Index);
                }
            }

            int32 Next = Constants::kIndexNone;
            for (uint16 Index = 0; Index < (uint16)Count; ++Index)
            {
                if (!Placed[Index] && Waiting[Index] == 0 && (Next == Constants::kIndexNone || Sooner(Index, (uint16)Next)))
                {
                    Next = Index;
                }
            }

            // Only a cycle leaves nothing ready, so it is broken at the system that would have run soonest.
            if (Next == Constants::kIndexNone)
            {
                FString Cycle;
                for (uint16 Index = 0; Index < (uint16)Count; ++Index)
                {
                    if (!Placed[Index])
                    {
                        Cycle += Cycle.IsEmpty() ? "" : ", ";
                        Cycle += Systems[Index]->GetClass()->GetName().ToString();
                        if (Next == Constants::kIndexNone || Sooner(Index, (uint16)Next))
                        {
                            Next = Index;
                        }
                    }
                }
                if (!bReportedCycle)
                {
                    LOG_ERROR("Entity systems {} declare a dependency cycle in stage {}, so part of their order there is ignored.",
                        Cycle, GUpdateStageNames[(uint8)Stage]);
                    bReportedCycle = true;
                }
            }

            Placed[Next] = 1;
            Order.Add((uint16)Next);
            for (uint16 Successor : Successors[Next])
            {
                Waiting[Successor] = Waiting[Successor] > 0 ? (uint16)(Waiting[Successor] - 1) : 0;
            }
        }

        CWorld::FStageSchedule Out;
        Out.Systems.Reserve(Count);
        TVector<uint16> Position(Count, 0);
        for (uint16 Index = 0; Index < (uint16)Count; ++Index)
        {
            Position[Order[Index]] = Index;
            Out.Systems.Add(Systems[Order[Index]]);
        }

        // Closed over the order, so a system with no work this frame still keeps the two around it in sequence.
        Out.Precedes.SetNum(Count * Count, 0);
        for (uint16 Index = 0; Index < (uint16)Count; ++Index)
        {
            for (uint16 Predecessor : Predecessors[Order[Index]])
            {
                const uint16 From = Position[Predecessor];
                if (From >= Index)
                {
                    continue;
                }
                Out.Precedes[From * Count + Index] = 1;
                for (uint16 Earlier = 0; Earlier < From; ++Earlier)
                {
                    if (Out.MustPrecede(Earlier, From))
                    {
                        Out.Precedes[Earlier * Count + Index] = 1;
                    }
                }
            }
        }
        return Out;
    }

    bool SystemSchedule::NeedsFlushBetween(const FSystemAccess& Earlier, const FSystemAccess& Later, bool bDeclaredOrder)
    {
        if (bDeclaredOrder || Earlier.bExclusive || Later.bExclusive)
        {
            return true;
        }
        return FSystemAccess::Intersects(Earlier.SentEvents, Later.Reads) || FSystemAccess::Intersects(Earlier.SentEvents, Later.Writes);
    }

    TVector<CWorld::FSystemSegment> SystemSchedule::BuildSegments(const CWorld::FStageSchedule& Stage, const TVector<uint8>& WithWork)
    {
        const int32 Count = Stage.Systems.Num();
        TVector<CWorld::FSystemSegment> Segments;
        TVector<int32>  SegmentOf(Count, Constants::kIndexNone);
        TVector<uint16> LocalOf(Count, 0);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (!WithWork[Index])
            {
                continue;
            }

            const FSystemAccess& Access = Stage.Systems[Index]->GetAccess();
            TVector<int32> Predecessors;
            int32 Target = 0;
            for (int32 Earlier = 0; Earlier < Index; ++Earlier)
            {
                if (SegmentOf[Earlier] == Constants::kIndexNone)
                {
                    continue;
                }

                const FSystemAccess& EarlierAccess = Stage.Systems[Earlier]->GetAccess();
                const bool bDeclaredOrder = Stage.MustPrecede(Earlier, Index);
                if (!bDeclaredOrder && !FSystemAccess::Conflicts(Access, EarlierAccess))
                {
                    continue;
                }

                const bool bFlush = SystemSchedule::NeedsFlushBetween(EarlierAccess, Access, bDeclaredOrder);
                Target = Math::Max(Target, SegmentOf[Earlier] + (bFlush ? 1 : 0));
                Predecessors.Add(Earlier);
            }

            if (Target == Segments.Num())
            {
                Segments.Emplace();
                Segments.Last().bExclusive = Access.bExclusive;
            }

            CWorld::FSystemSegment& Segment = Segments[Target];
            const uint16 Local = static_cast<uint16>(Segment.Members.Num());
            Segment.Members.Add(static_cast<uint16>(Index));
            Segment.Successors.Emplace();
            Segment.PredecessorCounts.Add(0);
            for (int32 Earlier : Predecessors)
            {
                if (SegmentOf[Earlier] == Target)
                {
                    Segment.Successors[LocalOf[Earlier]].Add(Local);
                    ++Segment.PredecessorCounts[Local];
                }
            }

            SegmentOf[Index] = Target;
            LocalOf[Index]   = Local;
        }
        return Segments;
    }

    TVector<CWorld::FSystemBatch> SystemSchedule::BuildLevels(const TVector<CWorld::FSystemSegment>& Segments)
    {
        TVector<CWorld::FSystemBatch> Levels;
        int32 Base = 0;
        for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
        {
            const CWorld::FSystemSegment& Segment = Segments[SegmentIndex];
            // Successors always come later in the member list, so one forward pass settles every depth.
            TVector<int32> Depth(Segment.Members.Num(), 0);
            int32 Deepest = 0;
            for (int32 Local = 0; Local < Segment.Members.Num(); ++Local)
            {
                Deepest = Math::Max(Deepest, Depth[Local]);
                for (uint16 Next : Segment.Successors[Local])
                {
                    Depth[Next] = Math::Max(Depth[Next], Depth[Local] + 1);
                }
            }

            Levels.SetNum(Base + Deepest + 1);
            for (int32 Local = 0; Local < Segment.Members.Num(); ++Local)
            {
                Levels[Base + Depth[Local]].Members.Add(Segment.Members[Local]);
                Levels[Base + Depth[Local]].Segment = SegmentIndex;
            }
            Base += Deepest + 1;
        }
        return Levels;
    }

    FString SystemSchedule::DescribeParallelism(const CWorld::FStageSchedule& Stage, const TVector<CWorld::FSystemSegment>& Segments, EUpdateStage StageId)
    {
        const TVector<CWorld::FSystemBatch> Batches = BuildLevels(Segments);
        int32 Scheduled = 0;
        int32 Widest = 0;
        FStringBuilder Widths;
        FStringBuilder Members;
        for (int32 BatchIndex = 0; BatchIndex < Batches.Num(); ++BatchIndex)
        {
            const CWorld::FSystemBatch& Batch = Batches[BatchIndex];
            Scheduled += Batch.Members.Num();
            Widest = Math::Max(Widest, (int32)Batch.Members.Num());
            Widths.AppendFormat(" {}", Batch.Members.Num());

            const bool bAfterFlush = BatchIndex > 0 && Batches[BatchIndex - 1].Segment != Batch.Segment;
            Members.AppendFormat("\n  {}{} [{}]", bAfterFlush ? "flush, " : "", BatchIndex, Batch.Members.Num());
            for (uint16 Index : Batch.Members)
            {
                const CEntitySystem* System = Stage.Systems[Index];
                Members.AppendFormat(" {}{}", System->GetClass()->GetName(), System->GetAccess().bExclusive ? "(exclusive)" : "");
            }
        }
        return Lumina::Format("{}: {} of {} systems with work, {} segments, batch widths{}, critical path {}, widest {}{}",
            GUpdateStageNames[(uint8)StageId], Scheduled, Stage.Systems.Num(), Segments.Num(), Widths.IsEmpty() ? " none" : Widths.View(),
            Batches.Num(), Widest, Members.View());
    }

    TVector<SystemSchedule::FAmbiguity> SystemSchedule::FindAmbiguities(const CWorld::FStageSchedule& Stage)
    {
        TVector<FAmbiguity> Ambiguities;
        const int32 Count = Stage.Systems.Num();
        for (int32 Second = 0; Second < Count; ++Second)
        {
            const FSystemAccess& SecondAccess = Stage.Systems[Second]->GetAccess();
            if (SecondAccess.bExclusive)
            {
                continue;
            }
            for (int32 First = 0; First < Second; ++First)
            {
                const FSystemAccess& FirstAccess = Stage.Systems[First]->GetAccess();
                uint32 ConflictId = 0;
                if (FirstAccess.bExclusive || Stage.MustPrecede(First, Second) || Stage.MustPrecede(Second, First)
                    || !FSystemAccess::FindConflictingId(FirstAccess, SecondAccess, ConflictId))
                {
                    continue;
                }
                Ambiguities.Emplace(First, Second, ConflictId);
            }
        }
        return Ambiguities;
    }

    void CWorld::RegisterSystems()
    {
        ScriptGeneration = DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
        RegisteredRealmKey = NetRealm::KeyOf(this);

        EntitySystems::CreateMissing(*this, DisabledSystems, Systems);
        RebuildSystemSchedule();
        StartupPendingSystems();

        // Registered by whichever world schedules first; later registrations of the same name are ignored.
        FConsoleRegistry::Get().RegisterCommand(FConsoleCommand("World.SystemParallelism",
            "Logs each world's last frame per stage, its system batches, their widths and the critical path in batches.",
            []
            {
                GWorldManager->ForEachWorld([](CWorld& World)
                {
                    LOG_INFO("World {}:\n{}", World.GetName(), World.DescribeSystemParallelism(false));
                });
            }));
    }

    void CWorld::RebuildSystemSchedule()
    {
        TVector<CEntitySystem*> InStage[(int32)EUpdateStage::Max];
        for (const TStrongObjectPtr<CEntitySystem>& System : Systems)
        {
            if (System == nullptr)
            {
                continue;
            }

            // A declared event read means a consumer exists, so producers that skip unheard events keep filling that queue.
            for (const uint32 TypeID : System->GetAccess().Reads)
            {
                if (TypeID < ECS::MaxComponentTypes)
                {
                    EventQueues.AddInterest((ECS::FComponentTypeID)TypeID);
                }
            }

            for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
            {
                if (System->RunsInStage((EUpdateStage)i))
                {
                    InStage[i].Add(System.Get());
                }
            }
        }

        for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
        {
            StageSchedules[i] = SystemSchedule::OrderStage(InStage[i], (EUpdateStage)i);

            #if !defined(LE_SHIPPING)
            const FStageSchedule& Stage = StageSchedules[i];
            for (const SystemSchedule::FAmbiguity& Ambiguity : SystemSchedule::FindAmbiguities(Stage))
            {
                const FName FirstName = Stage.Systems[Ambiguity.First]->GetClass()->GetName();
                const FName SecondName = Stage.Systems[Ambiguity.Second]->GetClass()->GetName();

                // Keyed by class name in either order, since a rebuild may reorder the pair or recreate a system as a new object.
                const size_t FirstHash = GetTypeHash(FirstName);
                const size_t SecondHash = GetTypeHash(SecondName);
                size_t Key = Math::Min(FirstHash, SecondHash);
                Hash::HashCombine(Key, Math::Max(FirstHash, SecondHash));
                Hash::HashCombine(Key, (size_t)Ambiguity.ConflictId);
                Hash::HashCombine(Key, (size_t)i);
                if (ReportedAmbiguities.TryAdd(Key))
                {
                    LOG_INFO("World: systems {} and {} both access {} in stage {} with no RunsAfter or RunsBefore between them, so registration order decides which runs first",
                        FirstName, SecondName, GetAccessTypeName(Ambiguity.ConflictId), GUpdateStageNames[i]);
                }
            }
            #endif

            // Built on the next tick, once each system has said whether it has work.
            SystemSegments[i].Reset();
            SystemsWithWork[i].Reset();
        }
    }

    void CWorld::StartupPendingSystems()
    {
        if (bSystemsStarted)
        {
            EntitySystems::StartupPending(SystemContext, Systems);
        }
    }

    // Read-only snapshot of the per-stage batch layout for the Gameplay Insights tool.
    void CWorld::GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const
    {
        Out.Reset();
        for (uint8 s = 0; s < (uint8)EUpdateStage::Max; ++s)
        {
            const FStageSchedule& Stage = StageSchedules[s];
            const TVector<FSystemBatch> Batches = SystemSchedule::BuildLevels(SystemSegments[s]);
            for (uint8 b = 0; b < (uint8)Batches.Num(); ++b)
            {
                for (uint16 Index : Batches[b].Members)
                {
                    const CEntitySystem* System = Stage.Systems[Index];
                    const FSystemAccess& Access = System->GetAccess();

                    FSystemScheduleEntry& Entry = Out.EmplaceGetRef();
                    Entry.Name       = System->GetClass()->GetName();
                    Entry.Stage      = (uint8)s;
                    for (const CEntitySystem* Other : Stage.Systems)
                    {
                        if (Other != System && SystemSchedule::DeclaresOrder(Other, System))
                        {
                            Entry.RunsAfter.Add(Other->GetClass()->GetName());
                        }
                    }
                    Entry.Batch      = b;
                    Entry.BatchSize  = (uint8)Batches[b].Members.Num();
                    Entry.bExclusive = Access.bExclusive;
                    Entry.bManaged   = Cast<CScriptClass>(System->GetClass()) != nullptr;
                    Entry.Writes     = Access.Writes;
                    Entry.Reads      = Access.Reads;
                }
            }
        }
    }

    FString CWorld::DescribeSystemParallelism(bool bAssumeAllWork) const
    {
        FStringBuilder Out;
        for (uint8 s = 0; s < (uint8)EUpdateStage::Max; ++s)
        {
            const FStageSchedule& Stage = StageSchedules[s];
            const TVector<FSystemSegment> Segments = bAssumeAllWork ? SystemSchedule::BuildSegments(Stage, TVector<uint8>(Stage.Systems.Num(), 1)) : SystemSegments[s];
            Out.AppendLine(SystemSchedule::DescribeParallelism(Stage, Segments, (EUpdateStage)s));
        }
        return Out.ToString();
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

        DisabledSystems = PendingDisabledSystems;

        // Torn down before the rebuild, so a disabled system still sees a schedule it belongs to.
        EntitySystems::DropDisabled(SystemContext, DisabledSystems, Systems);

        // Creates whatever was just re-enabled and starts only what has not started yet.
        RegisterSystems();
    }

    void CWorld::GetAllSystems(TVector<FSystemInfo>& Out) const
    {
        Out.Reset();

        EntitySystems::ForEachSystemClass([&](CClass* Class)
        {
            CEntitySystem* Described = Class->GetDefaultObject<CEntitySystem>();
            if (Described == nullptr || !Described->ShouldCreate())
            {
                return;
            }

            // The world's own instance first, since a C# Configure only runs on an object with a managed side, which a default object lacks.
            const auto Live = Algo::FindIf(Systems, [Class](const TStrongObjectPtr<CEntitySystem>& System) { return System.Get() != nullptr && System->GetClass() == Class; });
            if (Live != Systems.end())
            {
                Described = Live->Get();
            }
            else
            {
                // A disabled system has no instance, so its default object still reports the stages it would tick in.
                Described->ConfigureSystem();
            }

            FSystemInfo& Info = Out.EmplaceGetRef();
            Info.Class    = Class;
            Info.Name     = Class->GetName();
            Info.bEnabled = PendingDisabledSystems.Contains(Info.Name) == 0;

            for (uint8 i = 0; i < (uint8)EUpdateStage::Max; ++i)
            {
                if (Described->RunsInStage((EUpdateStage)i))
                {
                    Info.Stages.Add((EUpdateStage)i);
                }
            }
            Info.RunsAfter  = Described->GetRunsAfter();
            Info.RunsBefore = Described->GetRunsBefore();
        });
    }

    bool CWorld::IsSystemEnabled(TSubclassOf<CEntitySystem> System) const
    {
        return System.IsValid() && PendingDisabledSystems.Contains(System->GetName()) == 0;
    }

    void CWorld::SetSystemEnabled(TSubclassOf<CEntitySystem> SystemClass, bool bEnabled)
    {
        if (!SystemClass.IsValid())
        {
            return;
        }

        // Disabled systems persist by class name, since that survives a reload that remints the class.
        const FName System = SystemClass->GetName();

        const bool bCurrentlyEnabled = PendingDisabledSystems.Contains(System) == 0;
        if (bCurrentlyEnabled == bEnabled)
        {
            return;
        }

        if (bEnabled)
        {
            PendingDisabledSystems.Remove(System);
        }
        else
        {
            PendingDisabledSystems.Add(System);
        }

        // Persisted immediately; the system-list rebuild defers to ApplyPendingSystemChanges.
        SDefaultWorldSettings& Settings = GetDefaultWorldSettings();
        Settings.DisabledSystems.Reset();
        for (const FName& Name : PendingDisabledSystems)
        {
            Settings.DisabledSystems.Add(Name);
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

    FImmediateLineRenderer* CWorld::GetImmediateLines() const
    {
        if (IsSuspended() || RenderScene == nullptr)
        {
            return nullptr;
        }

        return RenderScene->GetImmediateLines();
    }

    void CWorld::DrainDebugTextLines(TVector<FDebugTextLine>& Out)
    {
        Out = Move(DebugTextLines);
        DebugTextLines.Reset();
    }

    EUpdateStage CWorld::GetUpdateStage() const
    {
        return SystemContext.GetUpdateStage();
    }

    ECS::FEntity CWorld::GetEntityByTag(const FName& Tag)
    {
        const ECS::FSparseSet* Storage = EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag);

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

    void CWorld::GetEntitiesByTag(const FName& Tag, TVector<ECS::FEntity>& Out)
    {
        // Iterating the storage yields components; the sparse-set base is what yields the entities.
        const ECS::FSparseSet* TagStorage =
            EntityRegistry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), Tag);

        if (TagStorage == nullptr)
        {
            return;
        }

        Out.Reserve(Out.Num() + TagStorage->Num());
        for (const ECS::FEntity Entity : *TagStorage)
        {
            if (!Entity.IsTombstone())
            {
                Out.Add(Entity);
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

    FName CWorld::GetEntityTag(ECS::FEntity Entity)
    {
        return ECS::Utils::GetEntityTag(EntityRegistry, Entity);
    }

    FName CWorld::GetEntityName(ECS::FEntity Entity)
    {
        const SNameComponent* Name = EntityRegistry.TryGet<SNameComponent>(Entity);
        return Name ? Name->Name : FName();
    }

    void CWorld::SetEntityTransform(ECS::FEntity Entity, const FTransform& NewTransform)
    {
        if (!EntityRegistry.IsValid(Entity))
        {
            return;
        }

        if (STransformComponent* Transform = EntityRegistry.TryGet<STransformComponent>(Entity))
        {
            Transform->SetLocalTransform(NewTransform);
            return;
        }
        EntityRegistry.Emplace<STransformComponent>(Entity, NewTransform);
        EntityRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
    }

    // Owned by RunSegment's frame, which outlives every task SyncWait runs over it.
    struct CWorld::FSegmentRun
    {
        CWorld*               World   = nullptr;
        const FStageSchedule* Stage   = nullptr;
        const FSystemSegment* Segment = nullptr;
        SSystemContext*       Context = nullptr;
        TVector<uint32>       Remaining;
    };

    // Whichever predecessor finishes last forks the system, so each one starts the moment its own inputs are ready.
    Jobs::TTask<void> CWorld::RunSegmentMember(FSegmentRun* Run, uint16 Local)
    {
        Run->World->RunScheduledSystem(*Run->Stage, Run->Segment->Members[Local], *Run->Context);
        for (uint16 Next : Run->Segment->Successors[Local])
        {
            if (std::atomic_ref<uint32>(Run->Remaining[Next]).fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                co_await Jobs::Fork(RunSegmentMember(Run, Next));
            }
        }
        co_await Jobs::Join();
    }

    Jobs::TTask<void> CWorld::RunSegmentRoots(FSegmentRun* Run)
    {
        for (uint16 Local = 0; Local < static_cast<uint16>(Run->Segment->Members.Num()); ++Local)
        {
            if (Run->Segment->PredecessorCounts[Local] == 0)
            {
                co_await Jobs::Fork(RunSegmentMember(Run, Local));
            }
        }
        co_await Jobs::Join();
    }

    void CWorld::RunScheduledSystem(const FStageSchedule& Stage, uint16 Index, SSystemContext& Context)
    {
        CEntitySystem* System = Stage.Systems[Index];

        // Named per system, so one that has no zones of its own still shows up in a capture.
        LUMINA_PROFILE_SECTION_NAMED(*System->GetClass()->GetName());

        // Stamped as it starts, since a reader ordered before it in the same segment must not see the write early.
        const FSystemAccess& Access = System->GetAccess();
        MarkDeclaredWrites(EntityRegistry, Access);

        // A system that declared its access shares the segment, so its structural changes wait for the segment to end.
        const ECS::FCommandBus::FDeferScope Defer(!Access.bExclusive, Index);
        const FSystemAccess* Outer = SetExecutingSystemAccess(&Access);
        System->OnUpdate(Context);
        SetExecutingSystemAccess(Outer);
    }

    void CWorld::RunSegment(const FStageSchedule& Stage, const FSystemSegment& Segment, SSystemContext& Context)
    {
        // Created here before going wide, since view() writes the shared pool map.
        for (uint16 Index : Segment.Members)
        {
            const FSystemAccess& MemberAccess = Stage.Systems[Index]->GetAccess();
            EntityRegistry.AssureStorages(MemberAccess.Pools);
            for (ECS::FComponentTypeID TypeID : MemberAccess.TrackedPools)
            {
                EntityRegistry.FindStorage(TypeID)->TrackChanges();
            }
        }

        FSegmentRun Run;
        Run.World   = this;
        Run.Stage   = &Stage;
        Run.Segment = &Segment;
        Run.Context = &Context;
        Run.Remaining.Reserve(Segment.PredecessorCounts.Num());
        for (uint16 Count : Segment.PredecessorCounts)
        {
            Run.Remaining.Add(Count);
        }
        Jobs::SyncWait(Jobs::EJobPriority::Normal, &CWorld::RunSegmentRoots, &Run);
    }

    void CWorld::TickSystems(SSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        const FStageSchedule& Stage = StageSchedules[(uint32)Context.GetUpdateStage()];

        FlushCommands();

        // After the flush, so work its commands created counts this frame.
        const EUpdateStage StageId = Context.GetUpdateStage();
        TVector<uint8>& WithWork = SystemsWithWork[(uint32)StageId];
        bool bWorkChanged = WithWork.Num() != Stage.Systems.Num();
        WithWork.SetNum(Stage.Systems.Num(), 0);
        for (int32 Index = 0; Index < Stage.Systems.Num(); ++Index)
        {
            const uint8 bHasWork = Stage.Systems[Index]->HasWork(Context) ? 1 : 0;
            bWorkChanged |= WithWork[Index] != bHasWork;
            WithWork[Index] = bHasWork;
        }
        if (bWorkChanged)
        {
            SystemSegments[(uint32)StageId] = SystemSchedule::BuildSegments(Stage, WithWork);
        }

        for (const FSystemSegment& Segment : SystemSegments[(uint32)StageId])
        {
            if (Segment.Members.Num() == 1)
            {
                RunScheduledSystem(Stage, Segment.Members[0], Context);
            }
            else
            {
                DEBUG_ASSERT(!Segment.bExclusive);
                RunSegment(Stage, Segment, Context);
            }

            FlushCommands();
        }
    }

    void CWorld::TickFixedUpdate(double FrameDeltaSeconds)
    {
        if (StageSchedules[(uint32)EUpdateStage::FixedUpdate].Systems.IsEmpty())
        {
            FixedUpdateAccumulator = 0.0f;
            return;
        }

        const SDefaultWorldSettings& Settings = GetDefaultWorldSettings();
        const float FixedDt  = 1.0f / Math::Max(10.0f, Settings.PhysicsHz);
        const int32 MaxSteps = (int32)Settings.MaxPhysicsSteps;
        if (MaxSteps <= 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // Clamped as a spiral-of-death guard, so a long hitch cannot queue a hundred steps.
        FixedUpdateAccumulator = Math::Min(FixedUpdateAccumulator + (float)FrameDeltaSeconds, (float)MaxSteps * FixedDt);

        const int32 Steps = Math::Min(MaxSteps, (int32)(FixedUpdateAccumulator / FixedDt));
        if (Steps <= 0)
        {
            return;
        }

        FixedUpdateAccumulator -= (float)Steps * FixedDt;

        const EUpdateStage OuterStage = SystemContext.UpdateStage;
        const double OuterDeltaTime   = SystemContext.DeltaTime;
        const double OuterRealDelta   = SystemContext.RealDeltaTime;
        const double OuterTime        = SystemContext.Time;

        SystemContext.UpdateStage   = EUpdateStage::FixedUpdate;
        SystemContext.DeltaTime     = FixedDt;
        SystemContext.RealDeltaTime = FixedDt;
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            FixedUpdateTime += FixedDt;
            SystemContext.Time = FixedUpdateTime;
            TickSystems(SystemContext);
        }

        SystemContext.UpdateStage   = OuterStage;
        SystemContext.DeltaTime     = OuterDeltaTime;
        SystemContext.RealDeltaTime = OuterRealDelta;
        SystemContext.Time          = OuterTime;
    }

    void CWorld::FlushCommands()
    {
        // Events go first, so a fan-out still finds the entity a command recorded after the send is about to destroy.
        for (int32 Round = 0; Round < 64; ++Round)
        {
            const bool bEvents = EventQueues.Commit();
            const bool bCommands = CommandBus.HasPending();
            if (bCommands)
            {
                LUMINA_PROFILE_SCOPE();
                CommandBus.Flush();
            }

            if (!bEvents && !bCommands)
            {
                break;
            }
        }

        // Settled here on one thread, so systems walking the parent links in parallel never rebuild the order under each other.
        EntityRegistry.GetHierarchy().EnsureOrder();
    }
}
