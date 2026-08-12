#pragma once

#include "Core/Object/Object.h"
#include "Core/UpdateContext.h"
#include "Core/Delegates/Delegate.h"
#include "World/Entity/Components/CameraComponent.h"
#include "Entity/Registry/EntityRegistry.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsScene.h"
#include "Entity/Systems/SystemContext.h"
#include "Scene/RenderScene/RenderScene.h"
#include "Scene/RenderScene/TexturePaintTypes.h"
#include "UI/WorldUIContext.h"
#include "Subsystems/TimerManager.h"
#include "Physics/Ray/RayCast.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "WorldTypes.h"
#include "Core/Functional/FunctionRef.h"
#include "Entity/Systems/EntitySystem.h"
#include "Entity/EntityHandle.h"
#include "World.generated.h"


namespace Lumina
{
    struct FAssetRef;
    struct SDefaultWorldSettings;
    struct FLineBatcherComponent;
    struct FTriangleBatcherComponent;
    struct FSimpleElementVertex;
    struct FWorldContext;
    class CTexture;
    class CTextureRenderTarget;
    class CEntityScript;
    class CWorld;
    class FImmediateLineRenderer;
    enum class ENetMode : uint8;

    namespace ECS
    {
        // Engine-internal raw registry access for whole-registry operations that have no per-op wrapper
        // (serialization, net replication, reflection meta-invoke). Gameplay, tooling, and C# go through
        // CWorld's typed component/entity/singleton wrappers instead -- the registry is not public API.
        RUNTIME_API FEntityRegistry& GetWorldRegistry(CWorld& World);
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
    // hands the Gameplay Insights editor tool. Reads/Writes are entt::type_hash ids; resolve names with
    // GetAccessTypeName (SystemAccess.h).
    struct FSystemScheduleEntry
    {
        FName           Name;                  // None for a managed (C#) system
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
        friend FEntityRegistry& ECS::GetWorldRegistry(CWorld&);

    public:
        
        // A system as scheduled in one stage.
        struct FStageSlot
        {
            FSystemFn      Update = nullptr;
            void*          Self = nullptr;
            FSystemAccess  Access;
            uint8          StagePriority = 255;
            FName          Name;            // for the Gameplay Insights schedule view (GetSystemSchedule)
        };

        // A unique active system in this world. Owns the once-per-system Startup/Teardown lifecycle; the
        // per-stage FStageSlots reference its Update. One entry per system regardless of how many stages
        // it ticks in.
        struct FActiveSystem
        {
            FName      Name;
            uint64     Hash = 0;
            FSystemFn  Startup = nullptr;
            FSystemFn  Teardown = nullptr;
            void*      Self = nullptr;
        };

        // One C#-authored system created for this world. Instance is a strong GCHandle (the FStageSlot
        // Self); Generation is the C# script generation it was created under, so a hot reload can drop
        // stale handles without touching a freed managed instance.
        struct FManagedSystem
        {
            void*        Instance = nullptr;
            EUpdateStage Stage = EUpdateStage::PrePhysics;
            int32        Priority = 128;
            int32        Generation = -1;
        };

        CWorld();

        //~ Begin CObject Interface
        void Serialize(FArchive& Ar) override;
        void PreLoad() override;
        void PostLoad() override;
        bool IsAsset() const override { return true; }
        //~ End CObject Interface
        
        /** Initializes systems and renderer. Must be called before anything is done with the world. */
        void InitializeWorld(EWorldType InWorldType);

        /** Shuts down the world; destroys systems, components, and entities. */
        void TeardownWorld();

        /** Runs systems attached to this world; called on every update stage. */
        void Update(const FUpdateContext& Context);

        // Steps physics. Game thread, between the DuringPhysics and PostPhysics stages.
        void TickPhysics();

        // Drains the contact events the step queued (entt::dispatcher). Called by
        // FWorldManager::TickPhysics right after the step; not a separate frame phase.
        void DispatchPhysicsEvents();

        // Read ECS to compute camera/post-process and populate the scene's per-frame state.
        // Must run before RenderView consumes it, same frame.
        void Extract();

        /**
         * Constructs an entity into the registry.
         * @param Name New name of the entity, not unique.
         * @param Transform Optional Transform.
         * @return a newly created entity.
         */
        FUNCTION(Script)
        entt::entity ConstructEntity(FName Name, const FTransform& Transform = FTransform());


        FUNCTION(Script)
        entt::entity SpawnPrefab(const FAssetRef& Prefab);

        /** Like SpawnPrefab(Path), but positions the spawned root at SpawnTransform and
         *  optionally reparents under Parent (entt::null = world root). */
        entt::entity SpawnPrefabAt(const FAssetRef& Prefab, const FTransform& SpawnTransform, entt::entity Parent = entt::null);

        // Shatter a destructible entity into physics-driven fragments. Origin = blast point;
        // Strength = outward launch m/s (0 uses ExplosionStrength). No-op without an unbroken SDestructibleComponent.
        bool FractureEntity(entt::entity Entity, const FVector3& Origin, float Strength = 0.0f);
        
        void SpawnPrefabAsync(const FName& Path, const TFunction<void(entt::entity)>& Callback);

        /** Spawns a projectile entity at Position moving at Velocity (world m/s). Damage rides along in
         *  the hit event; the entity auto-despawns after Lifetime seconds (0 = never); Instigator is
         *  ignored by the sweep so the shooter is never hit. Returns the new entity. Bind its hit with
         *  GetEntityRegistry().get<SProjectileComponent>(e).OnHit, or set more fields on that component. */
        FUNCTION(Script)
        entt::entity SpawnProjectile(FVector3 Position, FVector3 Velocity, float Damage, float Lifetime, entt::entity Instigator);

        // C++ convenience with defaults.
        entt::entity SpawnProjectile(FVector3 Position, FVector3 Velocity, float Damage = 0.0f, float Lifetime = 5.0f)
        {
            return SpawnProjectile(Position, Velocity, Damage, Lifetime, entt::null);
        }

        Physics::IPhysicsScene* GetPhysicsScene() const { return PhysicsScene.get(); }

        // Creates the physics scene if this world has none. Editor worlds skip it at init because Jolt
        // reserves hundreds of MB up front, so a tool that wants to actually simulate asks for one here.
        Physics::IPhysicsScene* EnsurePhysicsScene();
        
        STransformComponent& GetEntityTransform(entt::entity Entity);

        FUNCTION(Script)
        FVector3 GetEntityLocation(entt::entity Entity);

        FUNCTION(Script)
        void SetEntityLocation(entt::entity Entity, FVector3 Location);

        FUNCTION(Script)
        void SetEntityRotation(entt::entity Entity, FQuat Rotation);

        FUNCTION(Script)
        FVector3 TranslateEntity(entt::entity Entity, FVector3 Translation);

        FUNCTION(Script)
        uint32 GetNumEntities() const;
        
        SDefaultWorldSettings& GetDefaultWorldSettings();
        
        FUNCTION(Script)
        bool EntityHasTag(entt::entity Entity, const FName& Tag);

        FUNCTION(Script)
        entt::entity GetEntityByTag(const FName& Tag);

        FUNCTION(Script)
        entt::entity GetEntityByName(const FName& Name);

        FUNCTION(Script)
        FName GetEntityName(entt::entity Entity);

        TOptional<SRayResult> CastRay(const SRayCastSettings& Settings);

        TVector<SRayResult> CastSphere(const SSphereCastSettings& Settings) const;
        
        EUpdateStage GetUpdateStage() const;

        FTimerManager& GetTimerManager() { return EntityRegistry.ctx().get<FTimerManager>(); }
        const FTimerManager& GetTimerManager() const { return EntityRegistry.ctx().get<FTimerManager>(); }

        NODISCARD EWorldType GetWorldType() const { return WorldType; }

        /** The context this world belongs to. Non-null once the world has been registered via FWorldManager::CreateWorldContext. */
        NODISCARD FWorldContext* GetWorldContext() const { return OwningContext; }

        /** Shorthand for GetWorldContext()->NetMode; returns Standalone when no context is set. */
        NODISCARD ENetMode GetNetMode() const;

        /** True when this world is the network authority (listen or dedicated server). */
        NODISCARD bool IsNetServer() const;

        /** Server-side count of currently connected clients; 0 on clients and standalone worlds. */
        NODISCARD int32 GetConnectedClientCount() const;

        /** C#-facing debug-draw facade (World.Debug). */
        NODISCARD FWorldDebugInterface* GetDebugInterface() { return &DebugInterface; }
        
        entt::entity GetFirstEntityWith(entt::id_type Type);
        
        void DuplicateEntity(entt::entity& To, entt::entity From, const TFunctionRef<bool(entt::type_info)>& Callback);

        // Deep-copy Source and its children (components copy-constructed, transient handles rebuilt); returns the new root.
        FUNCTION(Script)
        entt::entity DuplicateEntity(entt::entity Source);

        // Reparent Child under Parent (Parent = null detaches to the world root), preserving world transform.
        FUNCTION(Script)
        void SetParent(entt::entity Child, entt::entity Parent);

        // Detach from the current parent, preserving world transform.
        FUNCTION(Script)
        void DetachFromParent(entt::entity Entity);

        FUNCTION(Script)
        entt::entity GetParent(entt::entity Entity);

        FUNCTION(Script)
        entt::entity GetRootEntity(entt::entity Entity);

        // --- Mesh sockets / bones. SocketOrBone accepts a socket name authored on the skeleton or
        // --- static mesh asset, or (skeletal only) a raw bone name.

        // Parent Child under Parent and keep it glued to the named socket/bone each frame
        // (adds an SSocketAttachmentComponent; the socket attachment system drives the transform).
        FUNCTION(Script)
        void AttachEntityToSocket(entt::entity Child, entt::entity Parent, const FName& SocketOrBone);

        // Stop following the socket and detach to the world root, preserving world transform.
        FUNCTION(Script)
        void DetachEntityFromSocket(entt::entity Entity);

        FUNCTION(Script)
        bool HasSocket(entt::entity Entity, const FName& SocketOrBone);

        /** World-space socket/bone location on the entity's skeletal mesh; zero when it doesn't resolve. */
        FUNCTION(Script)
        FVector3 GetSocketLocation(entt::entity Entity, const FName& SocketOrBone);

        /** World-space socket/bone rotation on the entity's skeletal mesh; identity when it doesn't resolve. */
        FUNCTION(Script)
        FQuat GetSocketRotation(entt::entity Entity, const FName& SocketOrBone);

        /** Bone name for a skeleton bone index (e.g. a hit result's BoneIndex); NAME_None when out of range. */
        FUNCTION(Script)
        FName GetBoneName(entt::entity Entity, int32 BoneIndex);

        FUNCTION(Script)
        int32 GetBoneIndex(entt::entity Entity, const FName& BoneName);

        /** Bone origin nearest WorldLocation; approximates the hit bone on single-body skeletal meshes. */
        FUNCTION(Script)
        FName FindClosestBone(entt::entity Entity, FVector3 WorldLocation);

        FUNCTION(Script)
        void DestroyEntity(entt::entity Entity);
        
        void SetActiveCamera(entt::entity InEntity) const;

        /** Switch the active camera, easing from the current view over BlendTime seconds (0 = snap). */
        void SetActiveCamera(entt::entity InEntity, float BlendTime, ECameraBlendFunction Function = ECameraBlendFunction::EaseInOut) const;

        FUNCTION(Script)
        SCameraComponent* GetActiveCamera() const;

        entt::entity GetActiveCameraEntity() const;
        
        void OnChangeCameraEvent(const FSwitchActiveCameraEvent& Event);
        
        FUNCTION(Script)
        double GetWorldDeltaTime() const { return DeltaTime; }

        FUNCTION(Script)
        double GetTimeSinceWorldCreation() const { return TimeSinceCreation; }
        

        /** Pauses gameplay (systems + physics). UI keeps updating (ticked from Extract), so a script-driven
         *  pause menu can still unpause; systems registered for EUpdateStage::Paused keep running too. */
        FUNCTION(Script)
        void SetPaused(bool bNewPause) { bPaused = bNewPause; }

        FUNCTION(Script)
        bool IsPaused() const { return bPaused; }

        /** World time scale (slow motion / speed up). Scales DeltaTime for systems, scripts, and physics. */
        FUNCTION(Script)
        void SetTimeDilation(float Dilation);

        FUNCTION(Script)
        float GetTimeDilation();

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

        bool IsSimulating() const { return WorldType == EWorldType::Simulation; }

        static CWorld* DuplicateWorld(CWorld* OwningWorld);

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

        const TVector<FStageSlot>& GetSystemsForUpdateStage(EUpdateStage Stage);

        // One reflected engine system, as surfaced to the World Editor's Systems panel.
        struct FSystemInfo
        {
            FName                   Name;
            bool                    bEnabled = true;
            TVector<EUpdateStage>   Stages;     // stages this system participates in
        };

        // Enumerate every reflected engine system (alphabetical by reflected name) plus whether it is
        // currently enabled for this world. Reflects the pending (intended) state, so a UI checkbox
        // updates instantly even though the actual system list rebuild is deferred to the next frame.
        void GetAllSystems(TVector<FSystemInfo>& Out) const;

        // Whether System (by reflected name) is enabled for this world (reads the pending state).
        bool IsSystemEnabled(FName System) const;

        // Enable/disable a system for this world. Persists to SDefaultWorldSettings immediately and
        // defers the live system-list rebuild to the start of the next frame (ApplyPendingSystemChanges),
        // so it is safe to call mid-frame. Applies to native systems only.
        void SetSystemEnabled(FName System, bool bEnabled);

        void OnRelationshipComponentDestroyed(entt::registry& Registry, entt::entity Entity);
        void OnRelationshipComponentConstruct(entt::registry& Registry, entt::entity Entity);
        void OnTransformComponentConstruct(entt::registry& Registry, entt::entity Entity);
        void OnCSharpScriptComponentDestroyed(entt::registry& Registry, entt::entity Entity);
        void OnWidgetComponentDestroyed(entt::registry& Registry, entt::entity Entity);
        void OnInputComponentConstruct(entt::registry& Registry, entt::entity Entity);

        // Attaches a script of the given class to an entity (emplacing SEntityScriptComponent if needed) and
        // binds it immediately. Returns the managed instance handle, or null on failure.
        CEntityScript* AddEntityScript(entt::entity Entity, FStringView ScriptClass);

        // Convenience that forwards to AddEntityScript.
        void SetEntityScript(entt::entity Entity, FStringView ScriptClass);

        void RegisterSystems();

        // Read-only snapshot of the per-stage parallel system batches + each system's declared access, for the
        // Gameplay Insights editor tool. Replays the TickSystems batching; main thread.
        void GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const;

        //~ Begin Debug Drawing
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
        //~ End Debug Drawing

        //~ Begin Render Target Painting
        // Stamp a soft radial brush of Color into Target at UV (0..1). RadiusUV is relative to the
        // longer side; Strength = center opacity; Hardness > 1 sharpens. Queued, run next frame (TexturePaintPass).
        void PaintRenderTarget(CTextureRenderTarget* Target, const FVector2& UV, float RadiusUV, const FVector4& Color, float Strength = 1.0f, float Hardness = 1.0f, CTexture* BrushMask = nullptr);

        /** Clear an entire render target to Color (queued; executed during the render phase). */
        void ClearRenderTarget(CTextureRenderTarget* Target, const FVector4& Color);

        /** Render-scene Extract drains the queued paint/clear ops into the frame snapshot. */
        void DrainRenderTargetPaints(TVector<FTexturePaintOp>& OutOps);
        //~ End Render Target Painting
        
        FORCEINLINE bool IsGameWorld() const { return WorldType == EWorldType::Game; }
        
        void SetEntityTransform(entt::entity Entity, const FTransform& NewTransform);

        const FSystemContext& GetSystemContext() const { return SystemContext; }
        
        
        template<typename TFunc>
        void ForEachUniqueSystem(TFunc&& Func);
        
        template<typename T, typename... TArgs>
        decltype(auto) EmplaceComponent(FEntity Entity, TArgs&&... Args);
        
        template<typename T>
        requires(!std::is_empty_v<T>)
        T& GetComponent(FEntity Entity);
        
        template<typename T>
        T* TryGetComponent(FEntity Entity);

        // --- Entity-registry wrappers ---------------------------------------------------------------
        // The registry object is intentionally not exposed publicly; gameplay (C++ and C#) and tooling go
        // through these. entt views / entities / signal sinks are still surfaced -- we hide the registry
        // handle, not entt itself.

        template<typename T, typename... TArgs>
        decltype(auto) EmplaceOrReplaceComponent(FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.emplace_or_replace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename... TArgs>
        T& GetOrEmplaceComponent(FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.get_or_emplace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename... TArgs>
        T& ReplaceComponent(FEntity Entity, TArgs&&... Args)
        {
            return EntityRegistry.replace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename TFunc>
        T& PatchComponent(FEntity Entity, TFunc&& Func)
        {
            return EntityRegistry.patch<T>(Entity, std::forward<TFunc>(Func));
        }

        template<typename... T>
        void RemoveComponent(FEntity Entity)
        {
            EntityRegistry.remove<T...>(Entity);
        }

        template<typename... T>
        NODISCARD bool HasComponent(FEntity Entity) const
        {
            return EntityRegistry.all_of<T...>(Entity);
        }

        template<typename... T>
        NODISCARD bool HasAnyComponent(FEntity Entity) const
        {
            return EntityRegistry.any_of<T...>(Entity);
        }

        template<typename T>
        const T& GetComponent(FEntity Entity) const
        {
            return EntityRegistry.get<T>(Entity);
        }

        template<typename T>
        const T* TryGetComponent(FEntity Entity) const
        {
            return EntityRegistry.try_get<T>(Entity);
        }

        template<typename T>
        void ClearComponents()
        {
            EntityRegistry.clear<T>();
        }

        NODISCARD bool IsValidEntity(FEntity Entity) const
        {
            return EntityRegistry.valid(Entity);
        }

        // Iteration. Returns the entt view directly; pass entt::exclude<...> for an exclusion set.
        template<typename... Get>
        NODISCARD auto View()
        {
            return EntityRegistry.view<Get...>();
        }

        template<typename... Get, typename... Exclude>
        NODISCARD auto View(entt::exclude_t<Exclude...> ExcludeSet)
        {
            return EntityRegistry.view<Get...>(ExcludeSet);
        }

        // Per-world singletons stored in the registry context.
        template<typename T, typename... TArgs>
        T& EmplaceSingleton(TArgs&&... Args)
        {
            return EntityRegistry.ctx().emplace<T>(std::forward<TArgs>(Args)...);
        }

        template<typename T>
        NODISCARD T& GetSingleton()
        {
            return EntityRegistry.ctx().get<T>();
        }

        template<typename T>
        NODISCARD const T& GetSingleton() const
        {
            return EntityRegistry.ctx().get<T>();
        }

        template<typename T>
        NODISCARD T* TryGetSingleton()
        {
            return EntityRegistry.ctx().find<T>();
        }

        template<typename T>
        NODISCARD const T* TryGetSingleton() const
        {
            return EntityRegistry.ctx().find<T>();
        }

        // Component lifecycle observers (entt signal sinks); connect member fns exactly as with entt.
        template<typename T> NODISCARD auto OnConstruct() { return EntityRegistry.on_construct<T>(); }
        template<typename T> NODISCARD auto OnDestroy()   { return EntityRegistry.on_destroy<T>(); }
        template<typename T> NODISCARD auto OnUpdate()    { return EntityRegistry.on_update<T>(); }
        NODISCARD auto OnEntityConstruct() { return EntityRegistry.on_construct<entt::entity>(); }
        NODISCARD auto OnEntityDestroy()   { return EntityRegistry.on_destroy<entt::entity>(); }

        // Low-level storage access for reflection-style passes (all storages) and named/tag storages.
        NODISCARD auto ComponentStorages() { return EntityRegistry.storage(); }

        template<typename T>
        NODISCARD auto& ComponentStorage() { return EntityRegistry.storage<T>(); }

        template<typename T>
        NODISCARD auto& NamedStorage(FEntityID Id) { return EntityRegistry.storage<T>(Id); }

        // Bare entity (no components); prefer ConstructEntity for a named/transformed entity.
        NODISCARD FEntity CreateEntity() { return EntityRegistry.create(); }

        // Destroys every entity in the world (component storages retain their types).
        void ClearAllEntities() { EntityRegistry.clear(); }

        template<typename T>
        NODISCARD bool HasSingleton() const { return EntityRegistry.ctx().contains<T>(); }

        template<typename T>
        void EraseSingleton() { EntityRegistry.ctx().erase<T>(); }

    private:

        // Raw registry handle. Intentionally private -- gameplay (C++/C#) and tooling use the typed wrappers
        // above; engine internals reach it through friendship (FSystemContext, FWorldManager, ...).
        FEntityRegistry& GetEntityRegistry() { return EntityRegistry; }
        const FEntityRegistry& GetEntityRegistry() const { return EntityRegistry; }

    private:

        void TickSystems(FSystemContext& Context);

        // Tears down + frees this world's C# system instances (respecting the generation guard: a stale
        // instance from a prior script generation is dropped, not destroyed, since managed already freed it).
        void DestroyManagedSystems();

        // Applies a deferred enable/disable request (set via SetSystemEnabled): tears down newly-disabled
        // systems, rebuilds the stage lists honoring DisabledSystems, then starts up newly-enabled ones.
        // Called at the top of Update() so it never runs inside a system batch. No-op unless bSystemsDirty.
        void ApplyPendingSystemChanges();

    private:
        
        FEntityRegistry                                     RegistryPending;
        FEntityRegistry                                     EntityRegistry;
        entt::dispatcher                                    SingletonDispatcher;
        entt::entity                                        SingletonEntity;

        FSystemContext                                      SystemContext;
        
        TUniquePtr<IRenderScene>                            RenderScene;
        TUniquePtr<Physics::IPhysicsScene>                  PhysicsScene;
        TUniquePtr<FWorldUIContext>                         UIContext;
        
        // Per-stage, priority-sorted update slots (direct-call fn-ptr + Self) consumed by TickSystems.
        TVector<FStageSlot>                                SystemUpdateList[(int32)EUpdateStage::Max];

        // Which of those slots may run together, as index lists into SystemUpdateList. A pure function of
        // the stage lists, so it is built once by RegisterSystems rather than per tick.
        TVector<TVector<uint16>>                           SystemBatches[(int32)EUpdateStage::Max];

        // Unique active systems in this world; owns Startup/Teardown lifecycle (one entry per system).
        TVector<FActiveSystem>                             ActiveSystems;

        // C#-authored systems created for this world (one managed instance each), scheduled into the
        // stage lists via the shared ManagedSystemUpdate shim. Destroyed on teardown / rebuild.
        TVector<FManagedSystem>                            ManagedSystems;

        // C# script generation the ManagedSystems were created under; a change (hot reload) triggers a
        // RegisterSystems rebuild so stale GCHandle slots are never ticked. -1 = none created yet.
        int32                                              ManagedSystemGeneration = -1;

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
        TConcurrentQueue<FTexturePaintOp>                   RenderTargetPaintQueue;

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
    
    
    
    
    
    
    
    
    
    

    template <typename TFunc>
    void CWorld::ForEachUniqueSystem(TFunc&& Func)
    {
        // ActiveSystems already holds exactly one entry per system.
        for (FActiveSystem& System : ActiveSystems)
        {
            Func(System);
        }
    }

    template <typename T, typename ... TArgs>
    decltype(auto) CWorld::EmplaceComponent(FEntity Entity, TArgs&&... Args)
    {
        return EntityRegistry.emplace<T>(Entity, std::forward<TArgs>(Args)...);
    }

    template <typename T>
    requires(!std::is_empty_v<T>)
    T& CWorld::GetComponent(FEntity Entity)
    {
        return EntityRegistry.get<T>(Entity);
    }

    template <typename T>
    T* CWorld::TryGetComponent(FEntity Entity)
    {
        return EntityRegistry.try_get<T>(Entity);
    }
}

