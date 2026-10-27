#pragma once

#include "Containers/BoundedQueue.h"
#include "Containers/Tuple.h"
#include "Containers/FunctionRef.h"
#include "Containers/Span.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Object.h"
#include "Core/Object/SubclassOf.h"
#include "Core/UpdateContext.h"
#include "Memory/SmartPtr.h"

#include "World/ECS/CommandBus.h"
#include "World/ECS/EventQueue.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/TexturePaintTypes.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "World/WorldContext.h"
#include "World/WorldDebugInterface.h"
#include "World/WorldTypes.h"

#include "Physics/PhysicsScene.h"
#include "Scripting/EntityScript.h"
#include "UI/WorldUIContext.h"

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Transform.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/ECS/Entity.h"

#include "World.generated.h"

namespace Lumina
{
    class CPhysicsSubsystem;
    class CInputSubsystem;
    class CTweenSubsystem;
    class CTimerSubsystem;
    class CMessageSubsystem;
    class CAnimationSubsystem;
    class CCameraSubsystem;
    class CNavigationSubsystem;
    class CPerceptionSubsystem;
    class CSequenceSubsystem;
    class CNetSubsystem;
    class CUISubsystem;
    class CDebugDrawSubsystem;
    class CSpawnSubsystem;
    class CEnvironmentSubsystem;
    class CSaveGameSubsystem;
    class CTexture;
    class CTextureRenderTarget;
    class FImmediateLineRenderer;
    struct FLineBatcherComponent;
    struct FResolvedSceneView;
    struct FTriangleBatcherComponent;
    struct SDefaultWorldSettings;
    struct SSceneFolderComponent;

    // One screen-space debug text line, drained by the render scene each frame.
    struct FDebugTextLine
    {
        FString  Text;
        FVector4 Color = FVector4(1.0f);
    };

    // One system as scheduled in a stage, as the Gameplay Insights tool shows it. Reads and Writes are component type ids.
    struct FSystemScheduleEntry
    {
        FName           Name;
        TVector<uint32> Writes;
        TVector<uint32> Reads;
        // The systems in this stage that its declarations make it wait for.
        TVector<FName>  RunsAfter;
        uint8           Stage      = 0;     // EUpdateStage
        uint8           Batch      = 0;     // Parallel batch index within the stage
        uint8           BatchSize  = 1;     // Systems running together in that batch
        bool            bExclusive = false;
        bool            bManaged   = false;
    };

    REFLECT()
    class RUNTIME_API CWorld : public CObject
    {
        GENERATED_BODY()

        friend class FWorldManager;
        friend struct SSystemContext;
        friend struct FWorldDebugInterface;
        friend struct SRenderComponent;

        // Defined only by tests, which drive systems by hand and need the context the world hands them.
        friend struct FWorldTestAccess;

    public:

        //~ Nested types.

        // One stage's systems in the order their declared dependencies give. Systems owns the instances and this only orders them.
        struct FStageSchedule
        {
            TVector<CEntitySystem*> Systems;

            // Row First, column Second, set when First has to finish before Second starts, directly or through other systems.
            TVector<uint8>          Precedes;

            bool MustPrecede(size_t First, size_t Second) const { return Precedes[First * Systems.size() + Second] != 0; }
        };

        // Systems that may run together, as indices into an FStageSchedule.
        struct FSystemBatch
        {
            TVector<uint16> Members;
        };

        // One reflected engine system as the World Editor's Systems panel lists it.
        struct FSystemInfo
        {
            TSubclassOf<CEntitySystem> Class;
            FName                 Name;
            bool                  bEnabled = true;
            TVector<EUpdateStage> Stages;
            // As declared, which a disabled system still reports.
            TVector<FName>        RunsAfter;
            TVector<FName>        RunsBefore;
        };

        //~ Construction and the CObject contract.

        CWorld();

        void Serialize(FArchive& Ar) override;

        // Both registries, so a world loaded but never initialized still shows what its entities hold.
        void VisitAdditionalObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;

        void PreLoad() override;
        void PostLoad() override;

        bool IsAsset() const override { return true; }

        static CWorld* DuplicateWorld(CWorld* OwningWorld);

        //~ Lifecycle, from creation through the per-frame stages to teardown.

        // Must run before anything else is done with the world.
        void InitializeWorld(EWorldType InWorldType);

        // Destroys systems, components and entities.
        void TeardownWorld();

        // Runs the systems of the current update stage.
        void Update(const FUpdateContext& Context);

        // Steps physics on the game thread, between the DuringPhysics and PostPhysics stages.
        void TickPhysics();

        // Sends the contact events the step recorded, right after it.
        void DispatchPhysicsEvents();

        // Resolves the camera and post process and fills the scene's frame state, before RenderView reads it.
        void Extract();

        void SetActive(bool bNewActive);

        bool IsSuspended() const { return !bActive; }

        // Seconds between frames while throttled, where 0 runs every frame.
        void SetUpdateInterval(double Seconds);

        double GetUpdateInterval() const { return UpdateIntervalSeconds; }

        // Latched once a frame in FWorldManager::BeginFrame, so every update stage of a frame agrees.
        bool IsTickingThisFrame() const { return bActive && !bThrottledThisFrame; }

        // Frees the render scene once suspended longer than GraceSeconds, returning whether it did.
        bool ReclaimIdleRenderer(double NowSeconds, double GraceSeconds);

        //~ Identity and networking.

        FUNCTION()
        NODISCARD EWorldType GetWorldType() const { return WorldType; }

        FUNCTION()
        NODISCARD bool IsGameWorld() const { return WorldType == EWorldType::Game; }

        FUNCTION()
        NODISCARD bool IsSimulating() const { return WorldType == EWorldType::Simulation; }

        // Set once FWorldManager::CreateWorldContext has registered the world.
        NODISCARD FWorldContext* GetWorldContext() const { return OwningContext; }
        
        FUNCTION()
        EUpdateStage GetUpdateStage() const;

        // Standalone when the world has no context.
        FUNCTION()
        NODISCARD ENetMode GetNetMode() const;

        // A load-test client, which gameplay drives itself instead of reading a person's input.
        FUNCTION()
        NODISCARD bool IsBotWorld() const;

        // True on a listen or dedicated server.
        FUNCTION()
        NODISCARD bool IsNetServer() const;

        // Zero on clients and standalone worlds.
        FUNCTION()
        NODISCARD int32 GetConnectedClientCount() const;

        // False headless, on a dedicated server and in a bot world, so cosmetic work can skip all three.
        FUNCTION()
        NODISCARD bool HasPresentation() const;

        //~ Time, pausing and dilation.

        FUNCTION()
        double GetWorldDeltaTime() const { return DeltaTime; }

        // The frame's time before dilation, for things such as cinematics that keep pace with the clock.
        FUNCTION()
        double GetRealDeltaTime() const { return RealDeltaTime; }

        FUNCTION()
        double GetTimeSinceWorldCreation() const { return TimeSinceCreation; }

        // Pauses systems and physics, while UI and systems in EUpdateStage::Paused keep running.
        FUNCTION()
        void SetPaused(bool bNewPause) { bPaused = bNewPause; }

        FUNCTION()
        bool IsPaused() const { return bPaused; }

        // Whether a paused editor world still previews animation, which an asset preview may not want.
        FUNCTION()
        void SetAnimatesWhilePaused(bool bAnimate) { bAnimatesWhilePaused = bAnimate; }

        FUNCTION()
        bool AnimatesWhilePaused() const { return bAnimatesWhilePaused; }

        // True from the start of teardown, so an OnDetach can tell the world going away from its entity being removed.
        FUNCTION()
        bool IsTearingDown() const { return bTearingDown; }

        // Scales the delta time systems, scripts and physics see.
        FUNCTION()
        void SetTimeDilation(float Dilation);

        FUNCTION()
        float GetTimeDilation() const;

        //~ Entities.

        // A named entity with a transform. The name need not be unique.
        FUNCTION()
        ECS::FEntity ConstructEntity(FName Name, const FTransform& Transform = FTransform());

        // An entity with no components at all.
        NODISCARD ECS::FEntity CreateEntity() { return EntityRegistry.Create(); }

        FUNCTION()
        void DestroyEntity(ECS::FEntity Entity);

        void DuplicateEntity(ECS::FEntity& To, ECS::FEntity From, const TFunctionRef<bool(const ECS::FComponentTypeInfo&)>& Callback);

        // Deep copies Source and its children, returning the new root.
        FUNCTION()
        ECS::FEntity DuplicateEntity(ECS::FEntity Source);

        // Destroys Entity after Seconds, where 0 never does. A second call retimes it.
        FUNCTION()
        void SetEntityLifetime(ECS::FEntity Entity, float Seconds);

        FUNCTION(SuppressGCTransition)
        NODISCARD bool IsValidEntity(ECS::FEntity Entity) const { return EntityRegistry.IsValid(Entity); }

        FUNCTION()
        uint32 GetNumEntities() const;

        // Destroys every entity while the component storages keep their types.
        void ClearAllEntities() { EntityRegistry.Clear(); }

        // Shatters a destructible entity from Origin, where Strength 0 uses the component's own.
        FUNCTION()
        bool FractureEntity(ECS::FEntity Entity, FVector3 Origin, float Strength = 0.0f);

        //~ Names and tags.

        FUNCTION()
        ECS::FEntity GetEntityByName(const FName& Name);

        FUNCTION()
        FName GetEntityName(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetEntityByTag(const FName& Tag);

        // None when the entity carries no tag.
        FUNCTION()
        FName GetEntityTag(ECS::FEntity Entity);

        // Appends every entity carrying Tag.
        FUNCTION()
        void GetEntitiesByTag(const FName& Tag, TVector<ECS::FEntity>& Out);

        FUNCTION()
        bool EntityHasTag(ECS::FEntity Entity, const FName& Tag);

        // An entity carries one tag, so this replaces whatever it had and None clears it.
        FUNCTION()
        void SetEntityTag(ECS::FEntity Entity, const FName& Tag);

        //~ Transforms. Getters read world space, and setters write the entity's own space under its parent.

        FUNCTION()
        FTransform GetEntityTransform(ECS::FEntity Entity);

        // Gives the entity a transform component when it has none.
        FUNCTION()
        void SetEntityTransform(ECS::FEntity Entity, const FTransform& NewTransform);

        FUNCTION()
        FVector3 GetEntityLocation(ECS::FEntity Entity);

        FUNCTION()
        FQuat GetEntityRotation(ECS::FEntity Entity);

        FUNCTION()
        FVector3 GetEntityScale(ECS::FEntity Entity);

        FUNCTION()
        void SetEntityLocation(ECS::FEntity Entity, FVector3 Location);

        FUNCTION()
        void SetEntityRotation(ECS::FEntity Entity, FQuat Rotation);

        FUNCTION()
        void SetEntityScale(ECS::FEntity Entity, FVector3 Scale);

        // One crossing and one lookup for placing an entity every frame.
        FUNCTION()
        void SetEntityLocationAndRotation(ECS::FEntity Entity, FVector3 Location, FQuat Rotation);

        FUNCTION()
        FVector3 TranslateEntity(ECS::FEntity Entity, FVector3 Translation);

        // One crossing for a pool of entities script moves every frame, pairing by index and ignoring extras.
        FUNCTION()
        void SetEntityTransforms(const ECS::FEntity* Entities, int32 EntityCount, const FTransform* Transforms, int32 TransformCount);

        //~ Hierarchy.

        // A null Parent detaches to the world root. The world transform is kept.
        FUNCTION()
        void SetParent(ECS::FEntity Child, ECS::FEntity Parent);

        FUNCTION()
        void DetachFromParent(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetParent(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetRootEntity(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity GetFirstChild(ECS::FEntity Entity) const;

        FUNCTION()
        ECS::FEntity GetNextSibling(ECS::FEntity Entity) const;

        // Entity first, then each parent up to the root, in one crossing.
        FUNCTION()
        void GetAncestorChain(ECS::FEntity Entity, TVector<ECS::FEntity>& Out) const;

        // Entity and every descendant, in no particular order.
        FUNCTION()
        void GetSubtree(ECS::FEntity Entity, TVector<ECS::FEntity>& Out) const;

        NODISCARD const ECS::FHierarchy& GetHierarchy() const { return EntityRegistry.GetHierarchy(); }

        //~ Entity scripts.

        // Attaches and binds a script of ScriptClass, C++ or C#, returning it or null on failure.
        FUNCTION()
        CEntityScript* AddEntityScript(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass);

        template<typename T>
        T* AddEntityScript(ECS::FEntity Entity);

        // The first script on Entity whose class derives ScriptClass, or null.
        FUNCTION()
        CEntityScript* FindEntityScript(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass);

        FUNCTION()
        void FindEntityScripts(ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass, TVector<TStrongObjectPtr<CEntityScript>>& Out);

        // Every attached script in the world whose class derives ScriptClass, in no particular order.
        FUNCTION()
        void FindScriptsInWorld(TSubclassOf<CEntityScript> ScriptClass, TVector<TStrongObjectPtr<CEntityScript>>& Out);

        // Runs OnDetach and drops the script, false when it was not attached to Entity.
        FUNCTION()
        bool RemoveEntityScript(ECS::FEntity Entity, CEntityScript* Script);

        //~ Components.

        template<typename T, typename... TArgs>
        decltype(auto) EmplaceComponent(ECS::FEntity Entity, TArgs&&... Args);

        template<typename T, typename... TArgs>
        decltype(auto) EmplaceOrReplaceComponent(ECS::FEntity Entity, TArgs&&... Args);

        template<typename T, typename... TArgs>
        T& GetOrEmplaceComponent(ECS::FEntity Entity, TArgs&&... Args);

        template<typename T, typename TFunc>
        T& PatchComponent(ECS::FEntity Entity, TFunc&& Func);

        template<typename T> requires (!std::is_empty_v<T>)
        NODISCARD auto& GetComponent(this auto& Self, ECS::FEntity Entity);

        template<typename T>
        NODISCARD auto* TryGetComponent(this auto& Self, ECS::FEntity Entity);

        template<typename... T>
        NODISCARD bool HasComponent(ECS::FEntity Entity) const;

        template<typename... T>
        NODISCARD bool HasAnyComponent(ECS::FEntity Entity) const;

        template<typename... T>
        void RemoveComponent(ECS::FEntity Entity);

        template<typename T>
        void ClearComponents();

        // Every storage, for reflection-style passes over all of them.
        NODISCARD auto& ComponentStorages() const { return EntityRegistry.GetActiveStorages(); }

        template<typename T>
        NODISCARD auto& ComponentStorage();

        // A named storage, such as the one per entity tag.
        template<typename T>
        NODISCARD auto& NamedStorage(uint32 Id);

        //~ Views and component signals.

        template<typename... TGet>
        NODISCARD auto View();

        template<typename... TGet, typename... TExcluded>
        NODISCARD auto View(ECS::TExclude<TExcluded...> Exclude);

        // Keeps CObject's destroy hook visible beside the component signal of the same name.
        using CObject::OnDestroy;

        template<typename T>
        NODISCARD ECS::FComponentSignal& OnConstruct();

        template<typename T>
        NODISCARD ECS::FComponentSignal& OnDestroy();

        template<typename T>
        NODISCARD ECS::FComponentSignal& OnUpdate();

        NODISCARD ECS::FComponentSignal& OnEntityConstruct() { return EntityRegistry.OnEntityCreated(); }

        NODISCARD ECS::FComponentSignal& OnEntityDestroy() { return EntityRegistry.OnEntityDestroyed(); }

        //~ Singletons, one per type in the registry context.

        template<typename T, typename... TArgs>
        T& EmplaceSingleton(TArgs&&... Args);

        template<typename T>
        NODISCARD auto& GetSingleton(this auto& Self);

        template<typename T>
        T& GetOrEmplaceSingleton();

        template<typename T>
        NODISCARD auto* TryGetSingleton(this auto& Self);

        template<typename T>
        NODISCARD bool HasSingleton() const;

        template<typename T>
        void EraseSingleton();

        //~ Registry, deferred commands and events.

        NODISCARD auto& GetEntityRegistry(this auto& Self);

        NODISCARD auto& GetCommandBus(this auto& Self);

        // Every event queue the world owns, sent to from anywhere and read by systems and scripts.
        NODISCARD auto& GetEvents(this auto& Self);

        // Commits sent events and applies recorded commands until both settle. Main thread, outside any batch.
        void FlushCommands();

        //~ Gameplay tags on entities.

        /** The registered tag ids the entity carries, empty when it has no tag component. */
        FUNCTION()
        void GetGameplayTags(ECS::FEntity Entity, TVector<uint32>& Out);

        //~ Stored on the tag component AddGameplayTag creates.

        FUNCTION()
        void AddGameplayTag(ECS::FEntity Entity, uint32 TagId);

        FUNCTION()
        void RemoveGameplayTag(ECS::FEntity Entity, uint32 TagId);

        /** Hierarchical, so an entity tagged with a leaf answers true for a query on its parent. */
        FUNCTION()
        bool HasGameplayTag(ECS::FEntity Entity, uint32 TagId);

        FUNCTION()
        bool HasGameplayTagExact(ECS::FEntity Entity, uint32 TagId);

        FUNCTION()
        void ClearGameplayTags(ECS::FEntity Entity);

        //~ Levels.

        // The swap runs at the next frame start, so calling this mid tick is safe.
        FUNCTION()
        void OpenLevel(const FString& Url);

        //~ Systems.

        void RegisterSystems();

        // Every reflected engine system by name, with its pending enabled state so a UI checkbox updates at once.
        void GetAllSystems(TVector<FSystemInfo>& Out) const;

        // Reads the pending state, so it answers a SetSystemEnabled made this frame.
        FUNCTION()
        bool IsSystemEnabled(TSubclassOf<CEntitySystem> System) const;

        // Persists at once and rebuilds the live system list at the start of the next frame, so it is safe mid-frame.
        FUNCTION()
        void SetSystemEnabled(TSubclassOf<CEntitySystem> System, bool bEnabled);

        // The last frame's per-stage batches with each system's declared access, for Gameplay Insights. Main thread.
        void GetSystemSchedule(TVector<FSystemScheduleEntry>& Out) const;

        //~ Subsystems, one per class per world, so a C++ and a C# one are the same thing to the world.

        // Matches a derived class too, and is null when the world has none.
        FUNCTION()
        NODISCARD CWorldSubsystem* GetSubsystem(TSubclassOf<CWorldSubsystem> Class) const { return WorldSubsystems::Find(Subsystems, Class.Get()); }

        template<typename T>
        NODISCARD T* GetSubsystem() const;

        NODISCARD const auto& GetSubsystems() const { return Subsystems; }

        // Creates the subsystem when the world has none yet, such as a world that was never initialized.
        FUNCTION()
        CWorldSubsystem* EnsureSubsystem(TSubclassOf<CWorldSubsystem> Class);

        template<typename T>
        T* EnsureSubsystem();

        //~ Engine subsystems, the grouped API, read in C# as World.Physics and the like. Never null.

        FUNCTION(ScriptProperty)
        CPhysicsSubsystem* GetPhysics();

        FUNCTION(ScriptProperty)
        CInputSubsystem* GetInput();

        FUNCTION(ScriptProperty)
        CTweenSubsystem* GetTweens();

        FUNCTION(ScriptProperty)
        CTimerSubsystem* GetTimers();

        FUNCTION(ScriptProperty)
        CMessageSubsystem* GetMessageBus();

        FUNCTION(ScriptProperty)
        CAnimationSubsystem* GetAnimation();

        FUNCTION(ScriptProperty)
        CCameraSubsystem* GetCamera();

        FUNCTION(ScriptProperty)
        CNavigationSubsystem* GetNavigation();

        FUNCTION(ScriptProperty)
        CPerceptionSubsystem* GetPerception();

        FUNCTION(ScriptProperty)
        CSequenceSubsystem* GetSequences();

        FUNCTION(ScriptProperty)
        CNetSubsystem* GetNet();

        FUNCTION(ScriptProperty)
        CUISubsystem* GetUI();

        FUNCTION(ScriptProperty)
        CDebugDrawSubsystem* GetDebugDraw();

        FUNCTION(ScriptProperty)
        CSpawnSubsystem* GetSpawner();

        FUNCTION(ScriptProperty)
        CEnvironmentSubsystem* GetEnvironment();

        FUNCTION(ScriptProperty)
        CSaveGameSubsystem* GetSaveGames();

        //~ Physics.

        Physics::IPhysicsScene* GetPhysicsScene() const { return PhysicsScene.get(); }

        // Editor worlds skip the physics scene at init since it reserves hundreds of MB, so a tool that simulates asks here.
        Physics::IPhysicsScene* EnsurePhysicsScene();

        // Fixed script updates ride the solver's step loop, since a force applied elsewhere is zeroed.
        void InstallPhysicsScriptHook();

        //~ Rendering.

        // Null when nothing has resolved a view this frame.
        const FResolvedSceneView* GetResolvedView() const;

        IRenderScene* GetRenderer() const { return RenderScene.get(); }

        // Both idempotent. The lifecycle calls them, and a renderer swap such as a C# render scene reload may too.
        void CreateRenderer();
        void DestroyRenderer();

        // False headless and on a dedicated server, which stays invisible even in the editor.
        NODISCARD bool ShouldRender() const;

        // Records Callback into the world's frame at every stage in Stages, until removed.
        FRenderCallbackHandle AddRenderCallback(ERenderStage Stages, FRenderCallback Callback, FName Name = "RenderCallback")
        {
            return RenderCallbacks.Add(Stages, Move(Callback), Name);
        }

        void RemoveRenderCallback(FRenderCallbackHandle& Handle) { RenderCallbacks.Remove(Handle); }

        NODISCARD auto& GetRenderCallbacks(this auto& Self);

        // The Rml context and documents, created in InitializeWorld and freed in TeardownWorld.
        FWorldUIContext* GetUIContext() const { return UIContext.get(); }

        //~ Render target painting, queued here and run during the next render.

        void EnqueueRenderTargetPaint(FTexturePaintOp&& Op);

        // A soft radial brush at UV, where RadiusUV is relative to the longer side and Hardness above 1 sharpens it.
        FUNCTION()
        void PaintRenderTarget(CTextureRenderTarget* Target, FVector2 UV, float RadiusUV, FVector4 Color, float Strength = 1.0f, float Hardness = 1.0f, CTexture* BrushMask = nullptr);

        FUNCTION()
        void ClearRenderTarget(CTextureRenderTarget* Target, FVector4 Color);

        // The render scene's Extract moves the queued paints into its frame snapshot.
        void DrainRenderTargetPaints(TVector<FTexturePaintOp>& OutOps);

        //~ Debug drawing, where the drawing calls themselves live on GetDebugDraw.

        NODISCARD FWorldDebugInterface* GetDebugInterface() { return &DebugInterface; }

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale);

        // Immediate line sink for large single-frame batches, when the world has a renderer.
        FImmediateLineRenderer* GetImmediateLines() const;

        // The render scene moves the queued text lines out each frame.
        void DrainDebugTextLines(TVector<FDebugTextLine>& Out);

        //~ Settings and scene folders.

        SDefaultWorldSettings& GetDefaultWorldSettings();

        // Saved with the world, and created on first use once the world is initialized.
        SWorldRenderSettings& GetRenderSettings();

        // Null before the world is initialized or before anything set them.
        NODISCARD auto* FindRenderSettings(this auto& Self);

        // The outliner's folder table, created on first use.
        SSceneFolderComponent& GetSceneFolders();

        // Null when the world has never had a folder table.
        NODISCARD auto* FindSceneFolders(this auto& Self);

        //~ Reflected state, public so the hot reload reinstancer and tools reach it.

        // One instance of every enabled system class, which the per-stage slots point into.
        PROPERTY(Hidden, NoSerialize)
        TVector<TStrongObjectPtr<CEntitySystem>> Systems;

        PROPERTY(Hidden, NoSerialize)
        TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;

    private:

        //~ Engine subsystem cache, refilled on the main thread whenever the list changes so workers only read it.

        using FEngineSubsystems = TTuple<CPhysicsSubsystem*, CInputSubsystem*, CTweenSubsystem*, CTimerSubsystem*,
            CMessageSubsystem*, CAnimationSubsystem*, CCameraSubsystem*, CNavigationSubsystem*, CPerceptionSubsystem*,
            CSequenceSubsystem*, CNetSubsystem*, CUISubsystem*, CDebugDrawSubsystem*, CSpawnSubsystem*,
            CEnvironmentSubsystem*, CSaveGameSubsystem*>;

        template<typename T>
        T* GetCachedSubsystem();

        void RefreshEngineSubsystems();

        //~ System scheduling.

        void TickSystems(SSystemContext& Context);

        // Rebuilds the per-stage slot lists and their parallel batches from Systems.
        void RebuildSystemSchedule();

        // Runs OnStartup on any system that has not had it, once the world's own startup pass has run.
        void StartupPendingSystems();

        // Applies a SetSystemEnabled request at the top of Update, so it never runs inside a batch.
        void ApplyPendingSystemChanges();

        // Latches whether this frame runs, from FWorldManager::BeginFrame.
        void AdvanceThrottle(double NowSeconds);

        //~ Registry signal handlers, connected in InitializeWorld.

        void OnTransformComponentConstruct(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnCSharpScriptComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnWidgetComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Logs Caller when Entity has no transform, for the script-facing transform setters.
        STransformComponent* FindScriptTransform(ECS::FEntity Entity, const char* Caller);

        //~ Entities and the buses that record against them, in construction order.

        ECS::FRegistry          RegistryPending;
        ECS::FRegistry          EntityRegistry;
        ECS::FEntity            SingletonEntity;

        // After the registry it records against, before the context that hands it out.
        ECS::FCommandBus        CommandBus;

        // After the registry its fan-outs deliver into, before the context that hands it out.
        ECS::FEventQueues       EventQueues;

        SSystemContext          SystemContext;

        FEngineSubsystems       EngineSubsystems;

        //~ Owned scenes.

        TUniquePtr<IRenderScene>            RenderScene;
        FRenderCallbackList                 RenderCallbacks;
        TUniquePtr<Physics::IPhysicsScene>  PhysicsScene;
        TUniquePtr<FWorldUIContext>         UIContext;

        //~ System schedule.

        // Per stage, the dependency-ordered systems TickSystems runs.
        FStageSchedule          StageSchedules[(int32)EUpdateStage::Max];

        // Which slots with work may run together, rebuilt only when that set changes between frames.
        TVector<FSystemBatch>   SystemBatches[(int32)EUpdateStage::Max];

        // Per slot, whether it had work when SystemBatches was built. Empty forces a rebuild.
        TVector<uint8>          SystemsWithWork[(int32)EUpdateStage::Max];

        // Set once the world has run its startup pass, which gates StartupPendingSystems.
        bool                    bSystemsStarted = false;

        // The C# script generation the scripted systems were built against, so a reload rebuilds them.
        int32                   ScriptGeneration = -1;

        // NetRealm::KeyOf when the systems were last registered.
        uint32                  RegisteredRealmKey = 0;

        // The applied and the requested disabled systems, which differ only until ApplyPendingSystemChanges.
        THashSet<FName>         DisabledSystems;
        THashSet<FName>         PendingDisabledSystems;
        bool                    bSystemsDirty = false;

        //~ Debug drawing and render target painting.

        FLineBatcherComponent*              LineBatcherComponent;
        FTriangleBatcherComponent*          TriangleBatcherComponent;
        TVector<FDebugTextLine>             DebugTextLines;
        TBoundedMPSCQueue<FTexturePaintOp>  RenderTargetPaintQueue;

        // Bound under World.Debug, with its World pointing back here.
        FWorldDebugInterface                DebugInterface;

        //~ Context, time and throttling.

        FWorldContext*          OwningContext = nullptr;

        double                  DeltaTime = 0.0;
        double                  RealDeltaTime = 0.0;
        double                  TimeSinceCreation = 0.0;

        // Engine clock time the world went suspended, or -1 while active, for the idle reclaim grace.
        double                  SuspendedTime = -1.0;

        // Interval 0 runs every frame, and NextUpdateTime is the engine clock deadline for the next one.
        double                  UpdateIntervalSeconds = 0.0;
        double                  NextUpdateTime = 0.0;

        uint32                  bPaused:1 = true;
        uint32                  bAnimatesWhilePaused:1 = true;
        uint32                  bActive:1 = true;
        uint32                  bThrottledThisFrame:1 = false;

        EWorldType              WorldType = EWorldType::None;
        bool                    bInitializing = true;
        bool                    bTearingDown = false;
    };

    // The scheduling CWorld::TickSystems runs, as free functions so it can be tested without a world.
    namespace SystemSchedule
    {
        // True when First's or Second's own declarations put First ahead of Second.
        RUNTIME_API bool DeclaresOrder(const CEntitySystem* First, const CEntitySystem* Second);

        // Declared dependencies first, then shared systems ahead of exclusive ones, then the class name, so the order never depends on discovery.
        RUNTIME_API CWorld::FStageSchedule OrderStage(const TVector<CEntitySystem*>& Systems, EUpdateStage Stage);

        // A system lands in the first batch after every earlier one it must follow or conflicts with, and systems without work are left out.
        RUNTIME_API TVector<CWorld::FSystemBatch> BuildBatches(const CWorld::FStageSchedule& Stage, const TVector<uint8>& WithWork);
    }
}

#include "World.inl"
