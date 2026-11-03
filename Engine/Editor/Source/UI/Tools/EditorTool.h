#pragma once

#include "EditorAction.h"
#include "ToolFlags.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/Cast.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "Events/EventProcessor.h"
#include "Memory/SmartPtr.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImViewGuizmo.h"
#include "Transactions/EditorTransaction.h"
#include "World/World.h"
#include "World/ECS/Registry.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Lumina
{
    class CEdNodeGraph;
    class CStruct;
    class FInputViewport;
    class FProperty;
    class FSceneEditorTool;
    class FUpdateContext;
    class IEditorToolContext;
    struct SCameraComponent;
}

namespace Lumina
{
    enum class EEditorCameraMode : uint8
    {
        Free,
        Orbit,
    };

    struct FEditorCameraState
    {
        EEditorCameraMode   Mode = EEditorCameraMode::Free;
        float               Speed = 50.0f;
        float               SpeedScale = 1.0f;
        FVector3            Velocity = FVector3(0.0f);

        // Degrees on +Z forward; a middle-drag pan moves OrbitTarget and ResetOrbitPan returns it to OrbitAnchor.
        FVector3            OrbitTarget = FVector3(0.0f);
        FVector3            OrbitAnchor = FVector3(0.0f);
        float               OrbitDistance = 5.0f;
        float               OrbitYaw = 0.0f;
        float               OrbitPitch = -15.0f;

        bool                bWasLooking = false;

        // A right press is the context-menu tap until the pointer travels, since capturing on press hides the release.
        bool                bRightLookArmed = false;
        float               RightLookTravel = 0.0f;

        // The Alt+LMB tumble pivot is latched for the whole drag, since recomputing it each frame crawls toward the camera.
        bool                bFreeOrbitActive = false;
        bool                bFreeOrbitPivotValid = false;
        FVector3            FreeOrbitPivot = FVector3(0.0f);
        float               FreeOrbitDistance = 10.0f;

        // A press with no camera modifier belongs to selection and the gizmo until it is released.
        bool                bLeftMouseDownPrev = false;
        bool                bLeftGestureBlocked = false;
        bool                bGizmoHovered = false;
        bool                bLeftDragGesture = false;

        bool                bHasLastFocusPoint = false;
        FVector3            LastFocusPoint = FVector3(0.0f);

        bool                bViewGizmoActive = false;
        FVector3            ViewGizmoPivot = FVector3(0.0f);

        // Ortho width is rebuilt from the view distance each frame, so toggling keeps the framing at the pivot.
        bool                bOrthographic = false;
        float               OrthoFreeDistance = 10.0f;

        bool                bFocusInterp = false;
        float               FocusInterpRate = 12.0f;
        FVector3            FocusFreePosition = FVector3(0.0f);
        FQuat               FocusFreeRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        FVector3            FocusOrbitTarget = FVector3(0.0f);
        float               FocusOrbitDistance = 5.0f;
    };

    // The only writer of ImGui's NoMouse, NoKeyboard and NoMouseCursorChange flags, derived from who owns input this frame.
    EDITOR_API void SyncImGuiInputFlags();

    class EDITOR_API FEditorTool : public IEventHandler, public Scripting::IScriptReloadListener
    {
        friend class FEditorUI;

    public:

        constexpr static char const* const ViewportWindowName = "Viewport";

        class FToolWindow
        {
            friend class FEditorTool;
            friend class FEditorUI;

        public:

            FToolWindow(const FName& InName, const TFunction<void(bool)>& InDrawFunction, const ImVec2& InWindowPadding = ImVec2(-1, -1), bool bInDisableScrolling = false)
                : Name(InName)
                , DrawFunction(InDrawFunction)
                , WindowPadding(InWindowPadding)
                , bDisableScrolling(bInDisableScrolling)
            {}

        protected:

            FName                   Name;
            TFunction<void(bool)>   DrawFunction;
            ImVec2                  WindowPadding;
            bool                    bViewport = false;
            bool                    bOpen = true;
            bool                    bDisableScrolling = false;
        };

        struct FViewportHit
        {
            FVector3        Location = FVector3(0.0f);
            FVector3        Normal = FVector3(0.0f, 1.0f, 0.0f);
            ECS::FEntity    Entity = ECS::NullEntity;
        };

        FEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld = nullptr);
        ~FEditorTool() override;
        LE_NO_COPYMOVE(FEditorTool);

        virtual void Initialize();
        virtual void Deinitialize(const FUpdateContext& UpdateContext);
        virtual void OnInitialize() = 0;
        virtual void OnDeinitialize(const FUpdateContext& UpdateContext) = 0;
        virtual void Update(const FUpdateContext& UpdateContext);
        virtual void WorldUpdate(const FUpdateContext& UpdateContext) {}
        virtual void EndFrame() {}
        void OnScriptsReloaded(const Scripting::FScriptReload& Reload) override {}

        // A tool is not a CObject, so whatever it holds outside the reflected graph is reported here; overrides call the base.
        virtual void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func);

        NODISCARD virtual uint32 GetUniqueTypeID() const = 0;
        NODISCARD virtual char const* GetUniqueTypeName() const = 0;
        NODISCARD virtual FName GetToolName() const { return ToolName; }
        NODISCARD virtual const char* GetTitlebarIcon() const { return LE_ICON_CAR_WRENCH; }
        NODISCARD virtual bool IsSingleton() const { return HasFlag(EEditorToolFlags::Tool_Singleton); }
        NODISCARD virtual bool IsSingleWindowTool() const { return false; }
        NODISCARD virtual bool ShouldOpenDocked() const { return true; }
        NODISCARD virtual bool IsAssetEditorTool() const { return false; }
        NODISCARD virtual bool HasLostAsset() const { return false; }
        NODISCARD virtual FFixedString GetAssetVirtualPath() const { return {}; }
        NODISCARD EEditorToolFlags GetToolFlags() const { return ToolFlags; }
        NODISCARD bool HasFlag(EEditorToolFlags Flag) const { return (ToolFlags & Flag) == Flag; }

        // The name's content hash rather than its index, since an ImGui dock class has to mean the same thing next session.
        NODISCARD uint64 GetID() const { return GetToolName().GetStableHash(); }

        NODISCARD ImGuiID CalculateDockspaceID() const;
        NODISCARD ImGuiID GetCurrentDockspaceID() const { return CurrDockspaceID; }
        NODISCARD ImGuiID GetPrevDockspaceID() const { return PrevDockspaceID; }
        NODISCARD ImGuiID GetDesiredDockID() const { return DesiredDockID; }
        NODISCARD ImGuiWindowClass* GetWindowClass() { return &ToolWindowsClass; }
        NODISCARD FFixedString GetToolWindowName(const FString& Name) const { return GetToolWindowName(Name.c_str(), CurrDockspaceID); }
        NODISCARD static FFixedString GetToolWindowName(char const* ToolWindowName, ImGuiID InDockspaceID);
        virtual void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const;

        NODISCARD CWorld* GetWorld() const { return World.Get(); }
        NODISCARD bool HasWorld() const { return World != nullptr; }
        NODISCARD virtual FSceneEditorTool* AsSceneEditor() { return nullptr; }

        // Replaces the world, tearing the old one down and setting up a fresh editor context.
        virtual void SetWorld(CWorld* InWorld);

        // Repoints at a world whose lifetime is owned elsewhere, such as a play session's.
        virtual void RebindToWorld(CWorld* InWorld);

        virtual void SetupWorldForTool();
        virtual void GetDefaultCameraPose(FVector3& OutLocation, FVector3& OutTarget) const;
        virtual ECS::FEntity CreateFloorPlane(float YOffset = 0.0f, float ScaleX = 20.0f, float ScaleY = 20.0f);

        NODISCARD virtual CEdNodeGraph* GetActiveNodeGraph() { return nullptr; }

        // Binds a canvas of this tool's asset that it has not opened yet, so edits made there join its history.
        virtual void AdoptNodeGraph(CEdNodeGraph* Graph) {}

        virtual void OnSave() {}
        NODISCARD virtual bool IsUnsavedDocument() { return false; }
        NODISCARD virtual bool HasUnsavedFileChanges() { return false; }
        virtual bool SaveFileChanges() { OnSave(); return !HasUnsavedFileChanges(); }
        virtual bool ShouldGenerateThumbnailOnSave() const { return false; }
        virtual void GenerateThumbnail(CPackage* Package);

        bool RunUndo();
        bool RunRedo();
        NODISCARD FName PeekUndoLabel() const { return TransactionManager.PeekUndoName(); }
        NODISCARD FName PeekRedoLabel() const { return TransactionManager.PeekRedoName(); }

        void RunObjectTransacted(FName Label, CObject* Object, const TFunction<void()>& Mutate);

        // A property set from outside the tool's panels, answered the way the tool answers its own panel edit.
        virtual void OnExternalPropertyEdit(CObject* Object, FProperty* Property) {}

        void DiscardUndoHistory() { ClearTransactionHistory(); }

        // Brackets an outside rewrite of this tool's world, such as a prefab pushing onto its instances, as one undo step.
        void BeginOutsideChange() { BeginTransaction(); }
        void EndOutsideChange(FName Label) { EndTransaction(Label); }

        void TickEditorCamera(double DeltaTime);
        FEditorCameraState& GetCameraState() { return CameraState; }
        const FEditorCameraState& GetCameraState() const { return CameraState; }
        void SetCameraMode(EEditorCameraMode Mode);
        NODISCARD bool IsEditorCameraOrthographic() const { return CameraState.bOrthographic; }
        void SetEditorCameraOrthographic(bool bOrthographic);
        NODISCARD float GetEditorViewDistance() const;
        void SetOrbitTarget(const FVector3& Target, float Distance = -1.0f);
        void ResetOrbitPan();
        void MoveEditorCameraBy(const FVector3& Delta);
        NODISCARD bool IsEditorCameraGestureActive() const { return CameraState.bLeftDragGesture; }

        // Viewport clicks yield to the camera while a gesture runs and while Alt is held over the viewport.
        NODISCARD bool ShouldSuppressViewportClickInput() const;

        // True when this tool, not the running game, drives the viewport camera and its gizmo, grid and focus companions.
        NODISCARD virtual bool HasEditorCameraControl() const { return HasWorld() && !World->IsGameWorld(); }

        virtual void FocusViewportToEntity(ECS::FEntity Entity);

        void UpdateViewportInput(const FUpdateContext& UpdateContext);
        virtual void DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture);
        virtual void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize);
        virtual void DrawViewportToolbar(const FUpdateContext& UpdateContext);

        // The panel area the world draws into, narrowed to the movie's aspect while one records so the edges are not cropped.
        static void GetViewportImageRect(ImVec2& OutMin, ImVec2& OutSize);

        // Drawn before the overlay, so ShouldSuppressViewportClickInput sees this frame's hover.
        void DrawViewGizmo(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);
        void DrawProjectionToggle(const ImVec2& Position, float Radius);
        NODISCARD virtual bool ShouldDrawViewGizmo() const { return true; }

        void DrawCameraModeSelector(float ItemWidth = 95.0f);
        void DrawGameFocusIndicator(ImVec2 ViewportSize);
        virtual void DrawWorldGrid();

        bool BeginViewportToolbarGroup(char const* GroupID, ImVec2 GroupSize, const ImVec2& Padding);
        void EndViewportToolbarGroup();

        // False when the bar should not draw, such as while the game has input; End must be called either way.
        bool BeginViewportToolbarWindow(float& OutButtonSize);
        void EndViewportToolbarWindow();

        NODISCARD bool IsViewportFullscreen() const { return bViewportFullscreen; }
        void SetViewportFullscreen(bool bInFullscreen) { bViewportFullscreen = bInFullscreen; }
        void ToggleViewportFullscreen() { bViewportFullscreen = !bViewportFullscreen; }

        // Where the cursor meets the world from rendered depth, read through foliage and Ignored; false outside the viewport.
        bool TraceViewportPlacement(ImVec2 ScreenPos, FViewportHit& OutHit, TSpan<const ECS::FEntity> Ignored = {}) const;
        bool ScreenToPickerPixel(ImVec2 ScreenPos, uint32& OutX, uint32& OutY) const;
        bool BuildViewportRay(ImVec2 ScreenPos, FVector3& OutOrigin, FVector3& OutDirection) const;

        void DrawViewModeButton(float ButtonSize);
        void DrawViewModePopupContents();
        void DrawViewModeMenu();

        NODISCARD virtual bool DrawsViewModeInViewportToolbar() const { return false; }
        virtual void DrawViewModeExtraItems() {}

        void DrawSkeletonDebug();
        void DrawSkeletonNameLabels(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);
        void DrawSkeletonDebugMenuItems();

        void DrawMainToolbar(const FUpdateContext& UpdateContext);
        virtual void DrawToolMenu(const FUpdateContext& UpdateContext) {}

        void DrawDrawerContent(bool bFocused);

        void RegisterAction(FEditorAction Action) { EditorActions.push_back(std::move(Action)); }
        const TVector<FEditorAction>& GetRegisteredActions() const { return EditorActions; }
        void DrawActionMenu(const char* MenuLabel, const char* Category);

        FTransform GetCameraSpawnTransform(float DistanceForward = 5.0f) const;

        FTransform GetDropSpawnTransform(CObject* Placed) const;

        // Hit lifted along its normal so Placed's mesh bounds, turned and scaled as given, rest on the surface.
        FTransform PlaceOnHit(const FViewportHit& Hit, CObject* Placed, const FQuat& Rotation = FQuat::Identity(), float Scale = 1.0f) const;

        void DrawDropPlacementMarker(const FViewportHit& Hit) const;
        CObject* LoadDroppedAsset(FStringView VirtualPath) const;

        // DropTarget is the entity under the drop, and bAttachToTarget whether the gesture meant to parent under it.
        ECS::FEntity HandleContentBrowserAssetDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget = false, const FTransform* SpawnTransform = nullptr);

        // Screen-space rect of the viewport image, refreshed each frame by UpdateViewportInput.
        ImVec2                      ViewportScreenMin = ImVec2(0.0f, 0.0f);
        ImVec2                      ViewportScreenSize = ImVec2(0.0f, 0.0f);

        THashSet<const CStruct*>    HiddenVisualizerTypes;
        bool                        bShowComponentVisualizers = true;
        bool                        bVisualizerHandlesEnabled = true;
        bool                        bShowHierarchyVisualizers = false;

    protected:

        // Play and Simulate run on a transient world that is discarded on stop, so only an editor world is transacted.
        bool CanTransact() const { return World != nullptr && World->GetWorldType() == EWorldType::Editor; }

        virtual void BeginTransaction();

        // Only these entities' transforms, so grabbing the gizmo in a large level does not serialize the registry.
        void BeginTransformTransaction(const TVector<ECS::FEntity>& Entities);

        // Records only what is created, so it must not see anything destroyed.
        void BeginCreationTransaction();

        void BeginComponentTransaction(const TVector<ECS::FEntity>& Entities, CStruct* ComponentType);
        void BeginRelationshipTransaction(const TVector<ECS::FEntity>& Seeds, ECS::FEntity NewParent = ECS::NullEntity);
        void BeginDestroyTransaction(const TVector<ECS::FEntity>& Doomed);
        void RecordComponentSnapshot(const TVector<ECS::FEntity>& Entities, CStruct* ComponentType);
        virtual void EndTransaction(FName Name);
        virtual void AbortTransaction();

        virtual void Undo();
        virtual void Redo();
        virtual bool AllowsUndoRedo() const { return true; }
        virtual void OnPostUndoRedo() {}
        bool CanUndo() const { return TransactionManager.CanUndo(); }
        bool CanRedo() const { return TransactionManager.CanRedo(); }
        void ClearTransactionHistory();
        FTransactionManager& GetTransactionManager() { return TransactionManager; }

        FToolWindow* CreateToolWindow(FName InName, const TFunction<void(bool)>& DrawFunction, const ImVec2& WindowPadding = ImVec2(-1, -1), bool DisableScrolling = false);
        void RemoveToolWindow(const FName& InName);

        virtual void DrawHelpMenu() { DrawHelpTextRow("No Help Available", ""); }
        void DrawHelpTextRow(const char* Label, const char* Text) const;

        void TickEditorActions();

        FTransactionManager                 TransactionManager;
        IEditorToolContext*                 ToolContext = nullptr;
        FName                               ToolName;
        TVector<TUniquePtr<FToolWindow>>    ToolWindows;
        TStrongObjectPtr<CWorld>            World;
        ECS::FEntity                        EditorEntity;
        FEditorCameraState                  CameraState;
        TUniquePtr<FInputViewport>          InputViewport;
        EEditorToolFlags                    ToolFlags = EEditorToolFlags::Tool_WantsToolbar;

        // The location is the dock node the tool sits in, or its window ID while it floats.
        ImGuiID                             CurrDockID = 0;
        ImGuiID                             DesiredDockID = 0;
        ImGuiID                             CurrLocationID = 0;
        ImGuiID                             PrevLocationID = 0;
        ImGuiID                             CurrDockspaceID = 0;
        ImGuiID                             PrevDockspaceID = 0;
        ImGuiWindowClass                    ToolWindowsClass;
        bool                                bInitialDockApplied = false;

        // True while an undo or redo destroys and recreates entities, so destroy observers keep their caches.
        bool                                bRestoringTransaction = false;
        bool                                bViewportFocused = false;
        bool                                bViewportHovered = false;
        bool                                bWorldGridEnabled = true;

        // Set by FEditorUI around Update for the tool that owns keyboard focus, so only its shortcuts fire.
        bool                                bIsActiveTool = false;

    private:

        void Internal_CreateViewportTool();
        void ApplyOrbitTransform();
        void BeginEditorLookCapture();
        void EndEditorLookCapture();
        void DrawKeybindsMenu();
        bool CanApplyHistory() const;
        const SCameraComponent* FindViewCamera() const;

        TVector<FEditorAction>              EditorActions;
        ImViewGuizmo::Context               ViewGizmoContext;
        bool                                bAssetReadOnlyOnDisk = false;
        bool                                bViewGizmoOrthoHovered = false;
        bool                                bViewportFullscreen = false;
    };
}

#define LUMINA_EDITOR_TOOL( TypeName ) \
constexpr static char const* const s_uniqueTypeName = #TypeName;\
constexpr static uint32 const s_toolTypeID = Lumina::Hash::FNV1a::GetHash32( #TypeName );\
constexpr static bool const s_isSingleton = false; \
virtual char const* GetUniqueTypeName() const override { return s_uniqueTypeName; }\
virtual uint32 GetUniqueTypeID() const override final { return TypeName::s_toolTypeID; }


#define LUMINA_SINGLETON_EDITOR_TOOL( TypeName ) \
constexpr static char const* const s_uniqueTypeName = #TypeName;\
constexpr static uint32 const s_toolTypeID = Lumina::Hash::FNV1a::GetHash32( #TypeName ); \
constexpr static bool const s_isSingleton = true; \
virtual char const* GetUniqueTypeName() const override { return s_uniqueTypeName; }\
virtual uint32 GetUniqueTypeID() const override final { return TypeName::s_toolTypeID; }\
virtual bool IsSingleton() const override final { return true; }
