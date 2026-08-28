#pragma once

#include "World/ECS/Registry.h"


#include "imgui.h"
#include "imgui_internal.h"
#include "EditorAction.h"
#include "ToolFlags.h"
#include "Transactions/EditorTransaction.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Events/EventProcessor.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImViewGuizmo.h"
#include "World/World.h"

namespace Lumina
{
    class FPrimitiveDrawManager;
    enum class EEditorToolFlags : uint8;
    class IEditorToolContext;
    class FUpdateContext;
    class FInputViewport;
    class CStruct;
}

namespace Lumina
{
    enum class EEditorCameraMode : uint8
    {
        Free,    // WASD + RMB-drag look (DCC-style flythrough)
        Orbit,   // RMB-drag yaw/pitch around a focal point, MMB pan, wheel zoom
    };

    // Per-tool editor-camera state; each tool ticks its own camera so mode/focus is per-editor.
    struct FEditorCameraState
    {
        EEditorCameraMode Mode = EEditorCameraMode::Free;

        float       Speed       = 50.0f;
        float       SpeedScale  = 1.0f;
        FVector3   Velocity    = FVector3(0.0f);

        // Yaw/pitch in degrees on +Z forward. OrbitAnchor is "home"; MMB-pan moves OrbitTarget, ResetOrbitPan snaps back.
        FVector3   OrbitTarget   = FVector3(0.0f);
        FVector3   OrbitAnchor   = FVector3(0.0f);
        float       OrbitDistance = 5.0f;
        float       OrbitYaw      = 0.0f;
        float       OrbitPitch    = -15.0f;

        // Trailing-edge: release captured mouse mode once on RMB-up, not every non-looking frame.
        bool        bWasLooking = false;

        // RMB alone is ambiguous on the press frame: a tap is the viewport's context-menu gesture, a drag
        // is a look. Capturing on press sets ImGuiConfigFlags_NoMouse, which clears ImGui's hovered window
        // for the rest of the gesture -- and the viewport's whole click block is behind IsWindowHovered(),
        // so the release that opens the menu was never seen. The look arms only once the pointer travels.
        bool        bRightLookArmed  = false;
        float       RightLookTravel  = 0.0f;

        // Free-mode Alt+LMB tumble. The pivot is captured on the gesture's rising edge and held for
        // the whole drag; recomputing it per frame makes the orbit crawl toward the camera.
        bool        bFreeOrbitActive     = false;
        bool        bFreeOrbitPivotValid = false;
        FVector3    FreeOrbitPivot       = FVector3(0.0f);
        float       FreeOrbitDistance    = 10.0f;

        // Left-button arbitration: a press made with no camera modifier belongs to selection/gizmo
        // and is latched out of camera gestures until release.
        bool        bLeftMouseDownPrev  = false;
        bool        bLeftGestureBlocked = false;
        // Published for the viewport picking/gizmo code, which draws later in the same frame.
        bool        bLeftDragGesture    = false;

        // Last point framed by F-focus; the Free-mode tumble borrows its distance.
        bool        bHasLastFocusPoint = false;
        FVector3    LastFocusPoint     = FVector3(0.0f);

        // Free-mode pivot for the view gizmo, latched for the whole drag for the same reason
        // FreeOrbitPivot is: recomputing it per frame makes the orbit crawl toward the camera.
        bool        bViewGizmoActive = false;
        FVector3    ViewGizmoPivot   = FVector3(0.0f);

        // Editor-only orthographic viewport. The ortho width is rebuilt each frame from the view
        // distance so toggling keeps the framing perspective had at the pivot, and so the existing
        // wheel zoom (which moves OrbitDistance) keeps working as a zoom.
        bool        bOrthographic = false;

        // Free mode has no orbit radius to zoom, and moving along forward is a no-op under a parallel
        // projection, so ortho zoom needs its own distance. Seeded from the pivot when ortho is enabled.
        float       OrthoFreeDistance = 10.0f;

        // Smooth focus interp; user movement input cancels mid-lerp.
        bool        bFocusInterp        = false;
        // Exponential-decay rate (1/s); ~12 yields ~250ms to ~95%.
        float       FocusInterpRate     = 12.0f;
        FVector3   FocusFreePosition   = FVector3(0.0f);
        FQuat   FocusFreeRotation   = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        FVector3   FocusOrbitTarget    = FVector3(0.0f);
        float       FocusOrbitDistance  = 5.0f;
    };

    // EDITOR_API: exported so editor plugins can derive their own tools out-of-module.
    class EDITOR_API FEditorTool : public IEventHandler
    {
    public:

        friend class FEditorUI;

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
            
            FName                 Name;
            TFunction<void(bool)> DrawFunction;
            ImVec2                WindowPadding;
            bool                  bViewport         = false;
            bool                  bOpen             = true;
            bool                  bDisableScrolling = false;
            
        };
        
        

    public:

        FEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld = nullptr);
        ~FEditorTool() override;
        LE_NO_COPYMOVE(FEditorTool);

        virtual void Initialize();
        virtual void Deinitialize(const FUpdateContext& UpdateContext);
        NODISCARD virtual FName GetToolName() const { return ToolName; }
        
        NODISCARD ImGuiID CalculateDockspaceID() const;

        NODISCARD FFixedString GetToolWindowName(const FString& Name) const { return GetToolWindowName(Name.c_str(), CurrDockspaceID); }
        
        NODISCARD ImGuiWindowClass* GetWindowClass() { return &ToolWindowsClass; }
        NODISCARD EEditorToolFlags GetToolFlags() const { return ToolFlags; }
        NODISCARD bool HasFlag(EEditorToolFlags Flag) const {  return (ToolFlags & Flag) == Flag; }

        NODISCARD CWorld* GetWorld() const { return World; }
        NODISCARD bool HasWorld() const { return World != nullptr; }
        NODISCARD ImGuiID GetCurrentDockspaceID() const { return CurrDockspaceID; }

        // Screen-space rect of the viewport image, refreshed each frame by UpdateViewportInput. Overlay code must use it rather than the inset ImGui cursor.
        ImVec2 ViewportScreenMin  = ImVec2(0.0f, 0.0f);
        ImVec2 ViewportScreenSize = ImVec2(0.0f, 0.0f);

        // Traces the world along the ray through ScreenPos and returns where an asset dropped there
        // should land. Falls back through terrain -> mesh bounds -> ground plane -> a fixed distance in
        // front of the camera, so it always produces something usable. Returns false only when there is
        // no camera to build a ray from.
        /** Cursor ray against terrain, then mesh bounds, then the ground plane; always yields a location.
         *  OutHitEntity (optional) receives the mesh entity that was hit, or ECS::NullEntity when the location
         *  came from terrain, the ground plane, or the fallback distance -- which is what lets a drop know
         *  whether it landed ON something. */
        bool TraceViewportPlacement(ImVec2 ScreenPos, FVector3& OutLocation, ECS::FEntity* OutHitEntity = nullptr) const;

        /** Cursor -> world ray through the tool's viewport rect. False when there is no camera. */
        bool BuildViewportRay(ImVec2 ScreenPos, FVector3& OutOrigin, FVector3& OutDirection) const;

        virtual void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const;
        
        virtual void OnInitialize() = 0;
        virtual void OnDeinitialize(const FUpdateContext& UpdateContext) = 0;
        
        virtual bool ShouldGenerateThumbnailOnSave() const { return false; }
        
        virtual void GenerateThumbnail(CPackage* Package);

        NODISCARD virtual bool IsSingleWindowTool() const { return false; }

        // When false, the tool's window opens floating instead of auto-docking into the main dockspace on
        // first appearance. The user can still dock it manually afterwards.
        NODISCARD virtual bool ShouldOpenDocked() const { return true; }

        NODISCARD virtual uint32 GetUniqueTypeID() const = 0;
        NODISCARD virtual char const* GetUniqueTypeName() const = 0;

        /** Replaces the world: destroys the old, creates a fresh editor context, sets up entities. */
        virtual void SetWorld(CWorld* InWorld);

        /** Pointer-only swap. World lifetime is owned elsewhere (e.g. PIE). */
        virtual void RebindToWorld(CWorld* InWorld);

        /** Called to set up the world for the tool */
        virtual void SetupWorldForTool();

        /** Where this tool's viewport camera starts, and what it points at. The default keeps the
         *  historical pose (slightly above the origin, looking straight down -Z) so tools that frame
         *  their own content are unaffected; the world editor overrides it to frame the default scene. */
        virtual void GetDefaultCameraPose(FVector3& OutLocation, FVector3& OutTarget) const;
        
        /** Creates a plane at world 0 */
        virtual ECS::FEntity CreateFloorPlane(float YOffset = 0.0f, float ScaleX = 10.0f, float ScaleY = 10.0f);
        
        /** Called just before updating the world at each stage */
        virtual void WorldUpdate(const FUpdateContext& UpdateContext) { }

        /** Per-frame update; overrides should call base (or TickEditorCamera) so look/orbit input works. */
        virtual void Update(const FUpdateContext& UpdateContext);

        /** Called once at the end of frame */
        virtual void EndFrame() { }
        
        /** Optionally draw a toolbar at the top of the window */
        void DrawMainToolbar(const FUpdateContext& UpdateContext);

        /** Draws the tool's registered tool-window content inline (no dockspace wrapper),
         *  for use inside the editor's footer drawers. */
        void DrawDrawerContent(bool bFocused);

        /** Drives the editor-entity camera; called from FEditorTool::Update. */
        void TickEditorCamera(double DeltaTime);

        /** True while a left-button camera drag (Alt+LMB orbit, LMB+RMB pan) is running this frame. */
        NODISCARD bool IsEditorCameraGestureActive() const { return CameraState.bLeftDragGesture; }

        /** Viewport click input (picking, marquee, gizmo grab) must yield to the camera: true while a
         *  gesture runs, and while Alt is merely held over this tool's viewport, since the user
         *  normally presses Alt before the mouse button. */
        NODISCARD bool ShouldSuppressViewportClickInput() const;

        FEditorCameraState&       GetCameraState()       { return CameraState; }
        const FEditorCameraState& GetCameraState() const { return CameraState; }

        /** Switch camera mode; entering Orbit derives target/yaw/pitch/distance from the current transform. */
        void SetCameraMode(EEditorCameraMode Mode);

        NODISCARD bool IsEditorCameraOrthographic() const { return CameraState.bOrthographic; }
        void SetEditorCameraOrthographic(bool bOrthographic);

        /** Distance the ortho framing and the view gizmo's free-mode pivot are derived from. */
        NODISCARD float GetEditorViewDistance() const;

        /** Re-anchor orbit on a new world point; updates OrbitTarget and the OrbitAnchor ResetOrbitPan returns to. */
        void SetOrbitTarget(const FVector3& Target, float Distance = -1.0f);

        /** Snap OrbitTarget back to OrbitAnchor (undo MMB-drag pan). */
        void ResetOrbitPan();

    private:

        /** Push current orbit state onto the editor entity's transform. */
        void ApplyOrbitTransform();

        void BeginEditorLookCapture();
        void EndEditorLookCapture();

    public:

        /** Free/Orbit combo for DrawViewportOverlayElements overrides. */
        void DrawCameraModeSelector(float ItemWidth = 95.0f);

        /** Allows the child to draw specific menu actions */
        virtual void DrawToolMenu(const FUpdateContext& UpdateContext) { }

        /** Viewport overlay to draw any elements to the window's viewport */
        virtual void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize);

        /** Blender-style orbit/dolly/pan gizmo pinned to the viewport's top-right. Call before the
         *  overlay so ShouldSuppressViewportClickInput sees this frame's hover state. */
        void DrawViewGizmo(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        /** Perspective/orthographic toggle, drawn as part of the view gizmo cluster. */
        void DrawProjectionToggle(const ImVec2& Position, float Radius);

        /** Opt a tool out of the view gizmo. Worlds in play and tools without an editor camera
         *  are already excluded. */
        NODISCARD virtual bool ShouldDrawViewGizmo() const { return true; }

        /** True when this tool -- not the running game -- drives the viewport camera, which is what
         *  gates the flycam's companions: the view gizmo, the world grid, F-to-focus, the ortho
         *  projection override, and the click-to-give-input-back-to-the-game handler. A game world
         *  normally means the game owns all of that; FWorldEditorTool overrides this so an EJECTED
         *  PIE session hands it back to the editor without also un-gating a normal play session. */
        NODISCARD virtual bool HasEditorCameraControl() const { return HasWorld() && !World->IsGameWorld(); }
        
        /** Draw the optional viewport for this tool window, returns true if focused. */
        virtual bool DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture);

        void UpdateViewportInput(const FUpdateContext& UpdateContext);

        /** Draws overlay elements on the viewport for tool actions. */
        virtual void DrawViewportToolbar(const FUpdateContext& UpdateContext);

        /** Amber outline + "Shift+F1: Editor focus" hint, shown when this tool's viewport currently owns
         *  game input. Shared by the world editor and game preview so focus reads consistently across tools.
         *  Call from DrawViewportOverlayElements. */
        void DrawGameFocusIndicator(ImVec2 ViewportSize);
        
        /** Moves the viewport to focus on the desired entity */
        virtual void FocusViewportToEntity(ECS::FEntity Entity);
        
        /** Draws an editor viewport grid if a world exists. Extent, spacing and color come from CViewportGridSettings. */
        virtual void DrawWorldGrid();

        /** Draws bone debug lines/joints for every skeletal mesh in the world (CVar-gated). */
        void DrawSkeletonDebug();

        /** Screen-space bone-name labels for every skeletal mesh (CVar-gated); called from DrawViewport. */
        void DrawSkeletonNameLabels(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        /** Skeleton-debug toggles; call inside a menu/popup. Backed by Editor.Debug.Skeleton*
         *  CVars so every surface stays in sync. */
        void DrawSkeletonDebugMenuItems();

        bool BeginViewportToolbarGroup(char const* GroupID, ImVec2 GroupSize, const ImVec2& Padding);
        void EndViewportToolbarGroup();

        /** Floating toolbar shell pinned to the viewport's top-left. Returns false when the bar should not
         *  draw (game input focused); End must be called either way. Lives here rather than on the scene
         *  tool so a preview-world tool gets the same chrome without duplicating it. */
        bool BeginViewportToolbarWindow(float& OutButtonSize);
        void EndViewportToolbarWindow();

        /** Eye button + "Visualizations" popup: render view modes, culling toggles, wireframe, and the
         *  physics/navigation/skeleton debug draws. Anything with a world can show it -- the settings it
         *  edits live on the world's render scene, not on the scene editor. */
        void DrawViewModeButton(float ButtonSize);

        /** The popup body without the button, so a tool can host it somewhere else (a menu, say). */
        void DrawViewModePopupContents();

        /** "Visualize" dropdown on the tool menu bar, beside Help. Drawn for any tool with a world that
         *  does not already surface this in its viewport toolbar. */
        void DrawViewModeMenu();

        /** True when the tool puts the view-mode control in its viewport toolbar instead (scene tools),
         *  which suppresses the menu-bar dropdown so there is only ever one entry point. */
        NODISCARD virtual bool DrawsViewModeInViewportToolbar() const { return false; }

        /** Hook: extra rows at the bottom of the View Mode menu (world: Game View, Entity Debug Info). */
        virtual void DrawViewModeExtraItems() {}

        /** Master toggle for component visualizers; read by the scene tool's EndFrame draw pass. */
        bool bShowComponentVisualizers = true;

        /** Toggle for the interactive pass, so face and radius handles can be muted without hiding the wireframe. */
        bool bVisualizerHandlesEnabled = true;

        /** Component types whose visualizer the user switched off in the Visualizations menu. */
        THashSet<const CStruct*> HiddenVisualizerTypes;

        /** Is this editor tool for editing assets? */
        NODISCARD virtual bool IsAssetEditorTool() const { return false; }

        /** VFS path of the asset this tool edits, or empty when the tool is not backed by one. */
        NODISCARD virtual FFixedString GetAssetVirtualPath() const { return {}; }
        
        /** Can there only ever be one of this tool? */
        NODISCARD virtual bool IsSingleton() const { return HasFlag(EEditorToolFlags::Tool_Singleton); }
        
        /** Optional title bar icon override */
        NODISCARD virtual const char* GetTitlebarIcon() const { return LE_ICON_CAR_WRENCH; }

        /** Called when the save icon is pressed. */
        virtual void OnSave() { }

        /** Called when the new icon is pressed */
        virtual void OnNew() { }
        
        NODISCARD virtual bool IsUnsavedDocument() { return false; }

        /** @TODO Cache and compare */
        NODISCARD uint64 GetID() const { return GetToolName().GetID(); }
        
        FORCEINLINE ImGuiID GetCurrDockID() const        { return CurrDockID; }
        FORCEINLINE ImGuiID GetDesiredDockID() const     { return DesiredDockID; }
        FORCEINLINE ImGuiID GetCurrLocationID() const    { return CurrLocationID; }
        FORCEINLINE ImGuiID GetPrevLocationID() const    { return PrevLocationID; }
        FORCEINLINE ImGuiID GetCurrDockspaceID() const   { return CurrDockspaceID; }
        FORCEINLINE ImGuiID GetPrevDockspaceID() const   { return PrevDockspaceID; }
        

        static FFixedString GetToolWindowName(char const* ToolWindowName, ImGuiID InDockspaceID)
        {
            DEBUG_ASSERT(ToolWindowName != nullptr);
            return FormatAs<FFixedString>("{}##{:08X}", ToolWindowName, InDockspaceID);
        }

    public:

        /** Returns a transform placed in front of the active editor camera by the given distance. */
        FTransform GetCameraSpawnTransform(float DistanceForward = 5.0f) const;

        /** Dispatches a content-browser asset drop by asset class. Returns the spawned entity (or ECS::NullEntity). */
        /** DropTarget is the entity the drop landed on (outliner row, or the viewport ray hit).
         *  bAttachToTarget is the separate question of whether the gesture meant to PARENT under it --
         *  true only for an outliner row drop; viewport placement acts on the target without adopting it. */
        ECS::FEntity HandleContentBrowserAssetDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget = false);

    protected:

        /** Undo/redo is only meaningful for an editable (Editor-type) world. Play/Simulate run on a
         *  transient PIE world that is discarded on stop, so transacting it is pointless and a full-registry
         *  serialize there causes a frame hitch (e.g. dragging the gizmo while simulating). */
        bool CanTransact() const { return World != nullptr && World->GetWorldType() == EWorldType::Editor; }

        /** Begin a transaction; captures before-state as a WHOLE-REGISTRY snapshot. */
        virtual void BeginTransaction();

        /**
         * Begin a transaction that records only these entities' transforms.
         *
         * Use this instead of BeginTransaction for any edit that is purely a transform. The general form
         * reflectively serializes the entire registry twice per transaction, which the gizmo cannot
         * afford: it opens the transaction on the frame a drag starts, so in a level carrying a foliage
         * component with hundreds of thousands of reflected instances that landed as a multi-hundred-
         * millisecond stall the moment the user grabbed something.
         */
        void BeginTransformTransaction(const TVector<ECS::FEntity>& Entities);

        /**
         * Begin a transaction for an operation that only ADDS entities (duplicate, paste, new entity).
         *
         * Records nothing up front and, at commit, serializes only what was created -- so the undo step
         * is sized by the addition rather than by the level. CREATION ONLY: an entity destroyed inside
         * one of these cannot be restored, and the command logs an error if it sees that happen.
         */
        void BeginCreationTransaction();

        /** Records one component type on these entities; falls back to whole-registry for an ops-less type. */
        void BeginComponentTransaction(const TVector<ECS::FEntity>& Entities, CStruct* ComponentType);

        /** Records the links, transforms and attachment state a hierarchy edit on Seeds can rewrite. */
        void BeginRelationshipTransaction(const TVector<ECS::FEntity>& Seeds, ECS::FEntity NewParent = ECS::NullEntity);

        /** Records the entities Doomed reaches, plus the links their surviving parents keep. */
        void BeginDestroyTransaction(const TVector<ECS::FEntity>& Doomed);

        /** Adds one component type to the transaction already open, for an edit that spans several. */
        void RecordComponentSnapshot(const TVector<ECS::FEntity>& Entities, CStruct* ComponentType);

        /** End a transaction; captures after-state and pushes onto the undo stack. */
        virtual void EndTransaction(FName Name);

        /** Discard the open transaction without committing (the "did nothing, cancel" paths). */
        virtual void AbortTransaction();

        virtual void Undo();
        virtual void Redo();

        /** Shared, domain-blind undo/redo manager; every tool records commands here. */
        FTransactionManager& GetTransactionManager() { return TransactionManager; }

        /** Undo/redo availability (all tools now record command-based transactions on the manager). */
        bool CanUndo() const { return TransactionManager.CanUndo(); }
        bool CanRedo() const { return TransactionManager.CanRedo(); }

        /** Whether undo/redo may run now; the world editor blocks it during PIE. */
        virtual bool AllowsUndoRedo() const { return true; }

        /** After a registry round-trip in Undo/Redo; override to rebuild caches mirroring registry state. */
        virtual void OnPostUndoRedo() { }

        /** Drops every transaction; call when the world is replaced or an asset reloads. */
        void ClearTransactionHistory();

        void Internal_CreateViewportTool();
        
        FToolWindow* CreateToolWindow(FName InName, const TFunction<void(bool)>& DrawFunction, const ImVec2& WindowPadding = ImVec2(-1, -1), bool DisableScrolling = false);

        /** Removes a previously-created tool window by name (no-op if absent). */
        void RemoveToolWindow(const FName& InName);
        
        /** Override to add tool-specific rows in a 2-column HelpTable. */
        virtual void DrawHelpMenu() { DrawHelpTextRow("No Help Available", ""); }

        void DrawHelpTextRow(const char* Label, const char* Text) const;

    private:

        /** Renders registered actions as Help > Keybinds; auto-called from DrawMainToolbar. */
        void DrawKeybindsMenu();

    public:

        /** Register a keybind-driven command; call from OnInitialize. Surfaces in Help > Keyboard Shortcuts. */
        void RegisterAction(FEditorAction Action) { EditorActions.push_back(std::move(Action)); }

        const TVector<FEditorAction>& GetRegisteredActions() const { return EditorActions; }

    protected:

        /** Polls action chords and fires callbacks; called from FEditorTool::Update; gated against text-input focus. */
        void TickEditorActions();

    private:

        TVector<FEditorAction>              EditorActions;
    
    protected:

        // Domain-blind command-based undo/redo shared by ALL tools (world editor, asset editors, ...).
        FTransactionManager                 TransactionManager;

        // True while an undo/redo restore destroys+recreates entities; entity-destroy observers no-op so caches survive.
        bool                                bRestoringTransaction = false;
        
        ImGuiID                             CurrDockID = 0;
        ImGuiID                             DesiredDockID = 0;      // The dock we wish to be in
        bool                                bInitialDockApplied = false; // One-shot: first-frame dock policy (e.g. force-undock) has run
        ImGuiID                             CurrLocationID = 0;     // Current Dock node we are docked into _OR_ window ID if floating window
        ImGuiID                             PrevLocationID = 0;     // Previous dock node we are docked into _OR_ window ID if floating window
        ImGuiID                             CurrDockspaceID = 0;    // Dockspace ID ~~ Hash of LocationID + DocType (with MYEDITOR_CONFIG_SAME_LOCATION_SHARE_LAYOUT=1)
        ImGuiID                             PrevDockspaceID = 0;
        ImGuiWindowClass                    ToolWindowsClass;       // All our tools windows will share the same WindowClass (based on ID) to avoid mixing tools from different top-level editor

        IEditorToolContext*                 ToolContext = nullptr;
        FName                               ToolName;
        
        TVector<TUniquePtr<FToolWindow>>    ToolWindows;
        
        TObjectPtr<CWorld>                  World;
        ECS::FEntity                        EditorEntity;
        FEditorCameraState                  CameraState;
        // Per-tool so a second visible viewport can't inherit this one's drag or snap animation.
        ImViewGuizmo::Context               ViewGizmoContext;
        ImTextureID                         SceneViewportTexture = 0;

        TUniquePtr<FInputViewport>          InputViewport;

        EEditorToolFlags                    ToolFlags = EEditorToolFlags::Tool_WantsToolbar;

        bool                                bViewportFocused = false;
        bool                                bViewportHovered = false;
        // The ortho toggle sits in the gizmo cluster, so it suppresses viewport clicks the same way.
        bool                                bViewGizmoOrthoHovered = false;

        // True only for the tool that currently owns keyboard focus. Set by FEditorUI immediately
        // before Update and cleared after, exactly like bViewportFocused. Registered-action shortcuts
        // read it: without it every open tool evaluates its own chords every frame, so the world
        // editor's Ctrl+S fired while a material graph had focus.
        bool                                bIsActiveTool = false;
		bool							    bWorldGridEnabled = true;

        // F11 fullscreen viewport: draws as borderless overlay; other tool windows are suppressed.
        bool                                bViewportFullscreen = false;

    public:
        NODISCARD bool IsViewportFullscreen() const { return bViewportFullscreen; }
        void SetViewportFullscreen(bool bInFullscreen) { bViewportFullscreen = bInFullscreen; }
        void ToggleViewportFullscreen() { bViewportFullscreen = !bViewportFullscreen; }
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
