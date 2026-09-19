#pragma once

#include "World/ECS/Registry.h"
#include "World/ECS/EventDispatcher.h"

#include "Containers/BoundedQueue.h"
#include "Core/Object/Object.h"
#include "Core/UpdateContext.h"
#include "Core/Delegates/Delegate.h"
#include "World/Entity/Components/CameraComponent.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsScene.h"
#include "Entity/Systems/SystemContext.h"
#include "Scene/RenderScene/RenderScene.h"
#include "Scene/RenderScene/TexturePaintTypes.h"
#include "UI/WorldUIContext.h"
#include "Subsystems/TimerManager.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/TweenManager.h"
#include "Physics/Ray/RayCast.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "WorldTypes.h"
#include "World/WorldContext.h"
#include "Containers/FunctionRef.h"
#include "Entity/Systems/EntitySystem.h"
#include "World.generated.h"

namespace Lumina
{
    struct FAssetRef;
    struct SDefaultWorldSettings;
    struct SSceneFolderComponent;
    struct FLineBatcherComponent;
    struct FTriangleBatcherComponent;
    struct FSimpleElementVertex;
    struct FWorldContext;
    struct FResolvedSceneView;
    class CTexture;
    class CTextureRenderTarget;
    class CEntityScript;
    class CParticleSystem;
    class CWorld;
    class FImmediateLineRenderer;

    namespace ECS
    {
        // Engine-internal raw registry access for whole-registry operations that have no per-op wrapper
        // (serialization, net replication, reflection meta-invoke). Gameplay, tooling, and C# go through
        // CWorld's typed component/entity/singleton wrappers instead -- the registry is not public API.
        RUNTIME_API ECS::FRegistry& GetWorldRegistry(CWorld& World);
    }
}

namespace Lumina
{
    // One queued screen-space debug-text line (DrawDebugText). Drained by the render scene each frame.
    struct FDebugTextLine
    {
        FString  Text;
        FVector4 Color = FVector4(1.0f);
    };
    
    // One system as scheduled in a stage, with its declared access, the snapshot CWorld::GetSystemSchedule
    // hands the Gameplay Insights editor tool. Reads/Writes are component type ids; resolve names with
    // GetAccessTypeName (SystemAccess.h).
    struct FSystemScheduleEntry
    {
        FName           Name;                  // the system's class name
        TVector<uint32> Writes;
        TVector<uint32> Reads;
        uint8           Stage      = 0;        // EUpdateStage
        uint8           Priority   = 255;
        uint8           Batch      = 0;        // parallel batch index within the stage
        uint8           BatchSize  = 1;        // systems running concurrently in this batch
        bool            bExclusive = false;
        bool            bManaged   = false;
    };

    struct FWorldDebugInterface
    {
        CWorld* World = nullptr;

        void DrawText(FStringView Text, TOptional<FVector4> Color);
        void DrawLine(FVector3 Start, FVector3 End, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawSphere(FVector3 Center, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
    };

    REFLECT()
    class RUNTIME_API CWorld : public CObject, public IPrimitiveDrawInterface
    {
        GENERATED_BODY()
        
        friend class FWorldManager;
        friend struct FSystemContext;
        friend struct SRenderComponent;
        friend ECS::FRegistry& ECS::GetWorldRegistry(CWorld&);

    public:

        //~ Nested types the public surface hands back.

        // One system as scheduled in one stage. Systems owns the instance; this only orders it.
        struct FStageSlot
        {
            CEntitySystem* System = nullptr;
            uint8          StagePriority = 255;
        };

        // One reflected engine system, as surfaced to the World Editor's Systems panel.
        struct FSystemInfo
        {
            FName                   Name;
            bool                    bEnabled = true;
            TVector<EUpdateStage>   Stages;     // stages this system participates in
        };

        //~ Construction and the CObject contract.

        CWorld();

        void Serialize(FArchive& Ar) override;

        void PreLoad() override;

        void PostLoad() override;

        bool IsAsset() const override { return true; }

        //~ Lifecycle, from world creation through per-frame stages to teardown.

        /** Initializes systems and renderer. Must be called before anything is done with the world. */
        void InitializeWorld(EWorldType InWorldType);

        /** Shuts down the world; destroys systems, components, and entities. */
        void TeardownWorld();

        /** Runs systems attached to this world; called on every update stage. */
        void Update(const FUpdateContext& Context);

        // Steps physics. Game thread, between the DuringPhysics and PostPhysics stages.
        void TickPhysics();

        // Drains the contact events the step queued (ECS::FEventDispatcher). Called by
        // FWorldManager::TickPhysics right after the step; not a separate frame phase.
        void DispatchPhysicsEvents();

        // Read ECS to compute camera/post-process and populate the scene's per-frame state.
        // Must run before RenderView consumes it, same frame.
        void Extract();

        void SetActive(bool bNewActive);

        bool IsSuspended() const { return !bActive; }

        /**
         * Seconds between this world's frames while throttled; 0 runs it every frame.
         */
        void SetUpdateInterval(double Seconds);

        double GetUpdateInterval() const { return UpdateIntervalSeconds; }

        /**
         * Whether this world runs its phases this frame. Decided once per frame in FWorldManager::BeginFrame
         * and read by every phase after it. Update runs seven times a frame (once per update stage), so a
         * gate re-evaluated per call could let a world through some stages and not others and half-tick it.
         */
        bool IsTickingThisFrame() const { return bActive && !bThrottledThisFrame; }

        // Frees the render scene once suspended longer than GraceSeconds. Returns true when it
        // actually reclaimed, so callers can budget one stall/frame.
        bool ReclaimIdleRenderer(double NowSeconds, double GraceSeconds);

        static CWorld* DuplicateWorld(CWorld* OwningWorld);

        //~ Identity and context.

        NODISCARD EWorldType GetWorldType() const { return WorldType; }

        /** The context this world belongs to. Non-null once the world has been registered via FWorldManager::CreateWorldContext. */
        NODISCARD FWorldContext* GetWorldContext() const { return OwningContext; }

        FORCEINLINE bool IsGameWorld() const { return WorldType == EWorldType::Game; }

        bool IsSimulating() const { return WorldType == EWorldType::Simulation; }

        EUpdateStage GetUpdateStage() const;

        const FSystemContext& GetSystemContext() const { return SystemContext; }

        /** Shorthand for GetWorldContext()->NetMode; returns Standalone when no context is set. */
        FUNCTION()
        NODISCARD ENetMode GetNetMode() const;

        /** True when this world is the network authority (listen or dedicated server). */
        FUNCTION()
        NODISCARD bool IsNetServer() const;

        /** Server-side count of currently connected clients; 0 on clients and standalone worlds. */
        FUNCTION()
        NODISCARD int32 GetConnectedClientCount() const;

        //~ World time, pausing and dilation.

        FUNCTION()
        double GetWorldDeltaTime() const { return DeltaTime; }

        FUNCTION()
        double GetTimeSinceWorldCreation() const { return TimeSinceCreation; }

        /** Pauses gameplay (systems + physics). UI keeps updating (ticked from Extract), so a script-driven
         *  pause menu can still unpause; systems registered for EUpdateStage::Paused keep running too. */
        FUNCTION()
        void SetPaused(bool bNewPause) { bPaused = bNewPause; }

        FUNCTION()
        bool IsPaused() const { return bPaused; }

        /** World time scale (slow motion / speed up). Scales DeltaTime for systems, scripts, and physics. */
        FUNCTION()
        void SetTimeDilation(float Dilation);

        FUNCTION()
        float GetTimeDilation();

        //~ Entity lifetime.

        /**
         * Constructs an entity into the registry.
         * @param Name New name of the entity, not unique.
         * @param Transform Optional Transform.
         * @return a newly created entity.
         */
        FUNCTION()
        ECS::FEntity ConstructEntity(FName Name, const FTransform& Transform = FTransform());

        // Bare entity (no components); prefer ConstructEntity for a named/transformed entity.
        NODISCARD ECS::FEntity CreateEntity() { return EntityRegistry.Create(); }

        FUNCTION()
        void DestroyEntity(ECS::FEntity Entity);

        void DuplicateEntity(ECS::FEntity& To, ECS::FEntity From, const TFunctionRef<bool(const ECS::FComponentTypeInfo&)>& Callback);

        // Deep-copy Source and its children (components copy-constructed, transient handles rebuilt); returns the new root.
        FUNCTION()
        ECS::FEntity DuplicateEntity(ECS::FEntity Source);

        /** Destroys Entity after Seconds (0 = never). Idempotent; a second call retimes the countdown. */
        FUNCTION()
        void SetEntityLifetime(ECS::FEntity Entity, float Seconds);

        FUNCTION(SuppressGCTransition)
        NODISCARD bool IsValidEntity(ECS::FEntity Entity) const
        {
            return EntityRegistry.IsValid(Entity);
        }

        FUNCTION()
        uint32 GetNumEntities() const;

        // Destroys every entity in the world (component storages retain their types).
        void ClearAllEntities() { EntityRegistry.Clear(); }

        ECS::FEntity GetFirstEntityWith(uint32 Type);

        // Shatter a destructible entity into physics-driven fragments. Origin = blast point;
        // Strength = outward launch m/s (0 uses ExplosionStrength). No-op without an unbroken SDestructibleComponent.
        bool FractureEntity(ECS::FEntity Entity, const FVector3& Origin, float Strength = 0.0f);

        //~ Entity naming and tags.

        FUNCTION()
        ECS::FEntity GetEntityByName(const FName& Name);

        FUNCTION()
        FName GetEntityName(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetEntityByTag(const FName& Tag);

        /** Appends every entity carrying Tag to Out. Tags are named storages, so this is a pool walk. */
        FUNCTION()
        void GetEntitiesByTag(const FName& Tag, TVector<ECS::FEntity>& Out);

        FUNCTION()
        bool EntityHasTag(ECS::FEntity Entity, const FName& Tag);

        //~ Entity transforms.

        STransformComponent& GetEntityTransform(ECS::FEntity Entity);

        void SetEntityTransform(ECS::FEntity Entity, const FTransform& NewTransform);

        FUNCTION()
        FVector3 GetEntityLocation(ECS::FEntity Entity);

        FUNCTION()
        void SetEntityLocation(ECS::FEntity Entity, FVector3 Location);

        FUNCTION()
        void SetEntityRotation(ECS::FEntity Entity, FQuat Rotation);

        FUNCTION()
        FVector3 TranslateEntity(ECS::FEntity Entity, FVector3 Translation);

        //~ Entity hierarchy.

        // Reparent Child under Parent (Parent = null detaches to the world root), preserving world transform.
        FUNCTION()
        void SetParent(ECS::FEntity Child, ECS::FEntity Parent);

        // Detach from the current parent, preserving world transform.
        FUNCTION()
        void DetachFromParent(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetParent(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetRootEntity(ECS::FEntity Entity);

        //~ Entity scripts.

        // Attaches a script of the given class to an entity (emplacing SEntityScriptComponent if needed) and
        // binds it immediately. Returns the managed instance handle, or null on failure.
        CEntityScript* AddEntityScript(ECS::FEntity Entity, FStringView ScriptClass);

        // Convenience that forwards to AddEntityScript.
        void SetEntityScript(ECS::FEntity Entity, FStringView ScriptClass);

        //~ Components.

        template<typename T, typename... TArgs>
        decltype(auto) EmplaceComponent(ECS::FEntity Entity, TArgs&&... Args);

        // The registry handle stays private; views, entities and signal sinks are reached through these.

        template<typename T, typename... TArgs>
        decltype(auto) EmplaceOrReplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.EmplaceOrReplace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename... TArgs>
        T& GetOrEmplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.GetOrEmplace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename... TArgs>
        T& ReplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.EmplaceOrReplace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename TFunc>
        T& PatchComponent(ECS::FEntity Entity, TFunc&& Func)
        {
            return EntityRegistry.Patch<T>(Entity, std::forward<TFunc>(Func));
        }

        template<typename T>
        requires(!std::is_empty_v<T>)
        T& GetComponent(ECS::FEntity Entity);

        template<typename T>
        const T& GetComponent(ECS::FEntity Entity) const
        {
            return EntityRegistry.Get<T>(Entity);
        }

        template<typename T>
        T* TryGetComponent(ECS::FEntity Entity);

        template<typename T>
        const T* TryGetComponent(ECS::FEntity Entity) const
        {
            return EntityRegistry.TryGet<T>(Entity);
        }

        template<typename... T>
        NODISCARD bool HasComponent(ECS::FEntity Entity) const
        {
            return EntityRegistry.HasAll<T...>(Entity);
        }

        template<typename... T>
        NODISCARD bool HasAnyComponent(ECS::FEntity Entity) const
        {
            return EntityRegistry.HasAny<T...>(Entity);
        }

        template<typename... T>
        void RemoveComponent(ECS::FEntity Entity)
        {
            EntityRegistry.Remove<T...>(Entity);
        }

        template<typename T>
        void ClearComponents()
        {
            EntityRegistry.ClearComponent<T>();
        }

        // Low-level storage access for reflection-style passes (all storages) and named/tag storages.
        NODISCARD auto ComponentStorages() { return EntityRegistry.GetActiveStorages(); }

        template<typename T>
        NODISCARD auto& ComponentStorage() { return EntityRegistry.GetStorage<T>(); }

        template<typename T>
        NODISCARD auto& NamedStorage(uint32 Id) { return EntityRegistry.GetStorage<T>(Id); }

        //~ Views and component signals.

        // Iteration. Returns the view directly; pass ECS::TExclude<...>{} for an exclusion set.
        template<typename... Get>
        NODISCARD auto View()
        {
            return EntityRegistry.View<Get...>();
        }

        template<typename... Get, typename... Exclude>
        NODISCARD auto View(ECS::TExclude<Exclude...> ExcludeSet)
        {
            return EntityRegistry.View<Get...>(ExcludeSet);
        }

        // Component lifecycle observers (pool signals); connect member functions by naming them.
        // The using-declaration keeps CObject's destroy hook visible alongside the component sink.
        using CObject::OnDestroy;

        template<typename T> NODISCARD auto& OnConstruct() { return EntityRegistry.GetSignals<T>().OnConstruct; }

        template<typename T> NODISCARD auto& OnDestroy()   { return EntityRegistry.GetSignals<T>().OnDestroy; }

        template<typename T> NODISCARD auto& OnUpdate()    { return EntityRegistry.GetSignals<T>().OnUpdate; }

        NODISCARD auto& OnEntityConstruct() { return EntityRegistry.OnEntityCreated(); }

        NODISCARD auto& OnEntityDestroy()   { return EntityRegistry.OnEntityDestroyed(); }

        //~ Singletons.

        // Per-world singletons stored in the registry context.
        template<typename T, typename... TArgs>
        T& EmplaceSingleton(TArgs&&... Args)
        {
            return EntityRegistry.Ctx().Emplace<T>(std::forward<TArgs>(Args)...);
        }

        template<typename T>
        NODISCARD T& GetSingleton()
        {
            return EntityRegistry.Ctx().Get<T>();
        }

        template<typename T>
        NODISCARD const T& GetSingleton() const
        {
            return EntityRegistry.Ctx().Get<T>();
        }

        template<typename T>
        T& GetOrEmplaceSingleton()
        {
            return EntityRegistry.Ctx().GetOrEmplace<T>();
        }

        template<typename T>
        NODISCARD T* TryGetSingleton()
        {
            return EntityRegistry.Ctx().Find<T>();
        }

        template<typename T>
        NODISCARD const T* TryGetSingleton() const
        {
            return EntityRegistry.Ctx().Find<T>();
        }

        template<typename T>
        NODISCARD bool HasSingleton() const { return EntityRegistry.Ctx().Contains<T>(); }

        template<typename T>
        void EraseSingleton() { EntityRegistry.Ctx().Erase<T>(); }

        //~ Systems.

        void RegisterSystems();

        // Enumerate every reflected engine system (alphabetical by reflected name) plus whether it is
        // currently enabled for this world. Reflects the pending (intended) state, so a UI checkbox
        // updates instantly even though the actual system list rebuild is deferred to the next frame.
        void GetAllSystems(TVector<FSystemInfo>& Out) const;

        // Whether System (by reflected name) is enabled for this world (reads the pending state).
        bool IsSystemEnabled(FName System) const;

        // Enable/disable a system for this world. Persists to SDefaultWorldSettings immediately and
        // defers the live system-list rebuild to the start of the next frame (ApplyPendingSystemChanges),
        // so it is safe to call mid-frame.
        void SetSystemEnabled(FName System, bool bEnabled);

        // Read-only snapshot of the per-stage parallel system batches + each system's declared access, for the
        // Gameplay Insights editor tool. Replays the TickSystems batching; main thread.
        void GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const;

        // One instance of every enabled system class; the per-stage FStageSlots only point into it.
        PROPERTY(NoSerialize)
        TVector<TObjectPtr<CEntitySystem>>                 Systems;

        //~ Subsystems. One CObject per subsystem class per world, so a C++ and a C# one are the same
        //~ thing to the world and to the details panel.

        // The subsystem of this class, or null when the world has none. Matches a derived class too.
        NODISCARD CWorldSubsystem* GetSubsystem(const CClass* Class) const
        {
            return WorldSubsystems::Find(Subsystems, Class);
        }

        template<typename T>
        NODISCARD T* GetSubsystem() const
        {
            return static_cast<T*>(GetSubsystem(T::StaticClass()));
        }

        NODISCARD const TVector<TObjectPtr<CWorldSubsystem>>& GetSubsystems() const { return Subsystems; }

        // Runtime state rather than map data, but reflected so the hot reload reinstancer reaches it.
        PROPERTY(NoSerialize)
        TVector<TObjectPtr<CWorldSubsystem>>               Subsystems;

        //~ Physics scene and queries.

        // C++ convenience with defaults.

        Physics::IPhysicsScene* GetPhysicsScene() const { return PhysicsScene.get(); }

        // Creates the physics scene if this world has none. Editor worlds skip it at init because the scene
        // reserves hundreds of MB up front, so a tool that wants to actually simulate asks for one here.
        Physics::IPhysicsScene* EnsurePhysicsScene();

        TOptional<SRayResult> CastRay(const SRayCastSettings& Settings);

        // OutHits is cleared and refilled near-to-far; reuse one buffer to keep repeated sweeps alloc-free.
        void CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) const;

        TOptional<SRayResult> CastSphereClosest(const SSphereCastSettings& Settings) const;

        //~ Camera and the resolved view.

        void SetActiveCamera(ECS::FEntity InEntity) const;

        /** Switch the active camera, easing from the current view over BlendTime seconds (0 = snap). */
        void SetActiveCamera(ECS::FEntity InEntity, float BlendTime, ECameraBlendFunction Function = ECameraBlendFunction::EaseInOut) const;

        FUNCTION()
        SCameraComponent* GetActiveCamera() const;

        ECS::FEntity GetActiveCameraEntity() const;

        void OnChangeCameraEvent(const FSwitchActiveCameraEvent& Event);

        /** The rendered view, or null when nothing has resolved one this frame. */
        const FResolvedSceneView* GetResolvedView() const;

        //~ Renderer and render targets.

        IRenderScene* GetRenderer() const { return RenderScene.get(); }

        // Creates/destroys this world's renderer (through RenderSceneFactory). Both are idempotent; the
        // world lifecycle calls them itself, but renderer swaps (e.g. a C# RenderScene hot reload) may
        // destroy and recreate on a live world.
        void CreateRenderer();

        void DestroyRenderer();

        // A world renders only when the process has a real RHI (not headless) and the world isn't a
        // dedicated server (which is invisible even in the editor). Gates RenderScene creation.
        NODISCARD bool ShouldRender() const;

        // Per-world UI (Rml context + documents); created in InitializeWorld, freed in TeardownWorld.
        FWorldUIContext* GetUIContext() const { return UIContext.get(); }

        void EnqueueRenderTargetPaint(FTexturePaintOp&& Op);

        // Stamp a soft radial brush of Color into Target at UV (0..1). RadiusUV is relative to the
        // longer side; Strength = center opacity; Hardness > 1 sharpens. Queued, run next frame (TexturePaintPass).
        void PaintRenderTarget(CTextureRenderTarget* Target, const FVector2& UV, float RadiusUV, const FVector4& Color, float Strength = 1.0f, float Hardness = 1.0f, CTexture* BrushMask = nullptr);

        /** Clear an entire render target to Color (queued; executed during the render phase). */
        void ClearRenderTarget(CTextureRenderTarget* Target, const FVector4& Color);

        /** Render-scene Extract drains the queued paint/clear ops into the frame snapshot. */
        void DrainRenderTargetPaints(TVector<FTexturePaintOp>& OutOps);

        //~ Debug drawing.

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override;

        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = -1.0f) override;

        /** Immediate-mode line sink, or null when this world has no renderer (dedicated server) or is
         *  suspended. Single frame, thickness 1, no CPU cull -- the path for the hundred-thousand-line
         *  cases. DrawLine above is still the one to use for timed or thick lines. */
        FImmediateLineRenderer* GetImmediateLines() const;

        /** Submit a solid triangle batch (3 pre-colored verts per tri). Duration <= 0 draws one frame.
         *  Mode picks the depth/blend state: Opaque for meshes that must occlude themselves, Translucent
         *  for blended overlays, XRay to ignore scene depth entirely. */
        void DrawSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode = ESolidDrawMode::Translucent, float Duration = -1.0f);

        /** Queue a line of screen-space debug text for this frame, stacked top-left on the world viewport */
        void DrawDebugText(const FString& Text, const FVector4& Color = FVector4(1.0f));

        /** Render scene drains the queued debug-text lines each frame (moves them out + clears). */
        void DrainDebugTextLines(TVector<FDebugTextLine>& Out);

        /** C#-facing debug-draw facade (World.Debug). */
        NODISCARD FWorldDebugInterface* GetDebugInterface() { return &DebugInterface; }

        //~ World settings and scene folders.

        SDefaultWorldSettings& GetDefaultWorldSettings();

        /** Outliner folder table for this world, created on first use. Editor-only organization. */
        SSceneFolderComponent& GetSceneFolders();

        /** The folder table without creating one, null when this world has never had one. */
        SSceneFolderComponent* FindSceneFolders();

        const SSceneFolderComponent* FindSceneFolders() const;

        //~ Timers and tweens.

        FTweenManager& GetTweenManager() { return EntityRegistry.Ctx().Get<FTweenManager>(); }

        const FTweenManager& GetTweenManager() const { return EntityRegistry.Ctx().Get<FTweenManager>(); }

        FTimerManager& GetTimerManager() { return EntityRegistry.Ctx().Get<FTimerManager>(); }

        const FTimerManager& GetTimerManager() const { return EntityRegistry.Ctx().Get<FTimerManager>(); }

        //~ Registry signal handlers, connected during world initialization.

        void OnRelationshipComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnRelationshipComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnTransformComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnCSharpScriptComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnWidgetComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

    private:

        // Raw registry handle. Intentionally private -- gameplay (C++/C#) and tooling use the typed wrappers
        // above; engine internals reach it through friendship (FSystemContext, FWorldManager, ...).
        ECS::FRegistry& GetEntityRegistry() { return EntityRegistry; }
        const ECS::FRegistry& GetEntityRegistry() const { return EntityRegistry; }

    private:

        void TickSystems(FSystemContext& Context);

        // Rebuilds the per-stage slot lists and their parallel batches from the current Systems list.
        void RebuildSystemSchedule();

        // Runs OnStartup on any system that has not had it yet, once the world's own startup pass has run.
        void StartupPendingSystems();

        // Applies a deferred enable/disable request (set via SetSystemEnabled): tears down newly-disabled
        // systems, rebuilds the stage lists honoring DisabledSystems, then starts up newly-enabled ones.
        // Called at the top of Update() so it never runs inside a system batch. No-op unless bSystemsDirty.
        void ApplyPendingSystemChanges();

    private:
        
        ECS::FRegistry                                     RegistryPending;
        ECS::FRegistry                                     EntityRegistry;
        ECS::FEventDispatcher                              SingletonDispatcher;
        ECS::FEntity                                       SingletonEntity;

        FSystemContext                                      SystemContext;
        
        TUniquePtr<IRenderScene>                            RenderScene;
        TUniquePtr<Physics::IPhysicsScene>                  PhysicsScene;
        TUniquePtr<FWorldUIContext>                         UIContext;
        
        // Per-stage, priority-sorted update slots (direct-call fn-ptr + Self) consumed by TickSystems.
        TVector<FStageSlot>                                SystemUpdateList[(int32)EUpdateStage::Max];

        // Which of those slots may run together, as index lists into SystemUpdateList. A pure function of
        // the stage lists, so it is built once by RegisterSystems rather than per tick.
        TVector<TVector<uint16>>                           SystemBatches[(int32)EUpdateStage::Max];

        // Set once the world has run its startup pass; gates StartupPendingSystems.
        bool                                               bSystemsStarted = false;

        // C# script generation the scripted systems were built against, so a hot reload can rebuild them.
        int32                                              ScriptGeneration = -1;

        // Reflected systems disabled for this world, by name. DisabledSystems is the applied state used by
        // RegisterSystems; PendingDisabledSystems is the editor-requested next state. They diverge only
        // between a SetSystemEnabled call and the next ApplyPendingSystemChanges (which reconciles them).
        THashSet<FName>                                     DisabledSystems;
        THashSet<FName>                                     PendingDisabledSystems;
        bool                                                bSystemsDirty = false;

        FLineBatcherComponent*                              LineBatcherComponent;
        FTriangleBatcherComponent*                          TriangleBatcherComponent;

        // Screen-space debug-text lines queued this frame (DrawDebugText); drained by the render scene.
        TVector<FDebugTextLine>                             DebugTextLines;

        // Render-target paint/clear requests; drained each Extract into the frame snapshot.
        TBoundedMPSCQueue<FTexturePaintOp>                  RenderTargetPaintQueue;

        FWorldContext*                                      OwningContext = nullptr;

        // C#-facing debug-draw facade bound under World.Debug; .World points back at this world.
        FWorldDebugInterface                                DebugInterface;
        double                                              DeltaTime = 0.0;
        double                                              TimeSinceCreation = 0.0;

        // Engine-clock time this world last went suspended; -1 while active. Drives idle-reclaim grace.
        double                                              SuspendedTime = -1.0;

        // Throttle state. Interval 0 means every frame; NextUpdateTime is the engine-clock deadline the
        // next frame is allowed at, and AdvanceThrottle (FWorldManager::BeginFrame) latches the decision.
        double                                              UpdateIntervalSeconds = 0.0;
        double                                              NextUpdateTime = 0.0;

        void AdvanceThrottle(double NowSeconds);

        uint32                                              bPaused:1 = true;
        uint32                                              bActive:1 = true;
        uint32                                              bThrottledThisFrame:1 = false;
        
        
        EWorldType                                          WorldType = EWorldType::None;
        bool                                                bInitializing = true;
    };
    
    
    
    
    
    
    
    
    
    

    template <typename T, typename ... TArgs>
    decltype(auto) CWorld::EmplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
    {
        return EntityRegistry.Emplace<T>(Entity, std::forward<TArgs>(Args)...);
    }

    template <typename T>
    requires(!std::is_empty_v<T>)
    T& CWorld::GetComponent(ECS::FEntity Entity)
    {
        return EntityRegistry.Get<T>(Entity);
    }

    template <typename T>
    T* CWorld::TryGetComponent(ECS::FEntity Entity)
    {
        return EntityRegistry.TryGet<T>(Entity);
    }
}

