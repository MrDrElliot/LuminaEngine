#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "EditorAction.h"
#include "ToolFlags.h"
#include "Transactions/EditorTransaction.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Events/EventProcessor.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "World/World.h"

namespace Lumina
{
    class FPrimitiveDrawManager;
    enum class EEditorToolFlags : uint8;
    class IEditorToolContext;
    class FUpdateContext;
    class FInputViewport;
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

            FToolWindow(const FName& InName, const TFunction<void(bool)>& InDrawFunction, const ImVec2& InWindowPadding = ImVec2(-1, -1), bool bDisableScrolling = false)
                : Name(InName)
                , DrawFunction(InDrawFunction)
                , WindowPadding(InWindowPadding)
            {}
        
        protected:
            
            FName                 Name;
            TFunction<void(bool)> DrawFunction;
            ImVec2                WindowPadding;
            bool                  bViewport = false;
            bool                  bOpen = true;
            
        };
        
        

    public:

        FEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld = nullptr);
        virtual ~FEditorTool();
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
        
        /** Creates a plane at world 0 */
        virtual entt::entity CreateFloorPlane(float YOffset = 0.0f, float ScaleX = 10.0f, float ScaleY = 10.0f);
        
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

        FEditorCameraState&       GetCameraState()       { return CameraState; }
        const FEditorCameraState& GetCameraState() const { return CameraState; }

        /** Switch camera mode; entering Orbit derives target/yaw/pitch/distance from the current transform. */
        void SetCameraMode(EEditorCameraMode Mode);

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
        virtual void FocusViewportToEntity(entt::entity Entity);
        
        /** Draws an editor viewport grid if a world exists */
        virtual void DrawWorldGrid(int Scale = 100, int Spacing = 1);

        /** Draws bone debug lines/joints for every skeletal mesh in the world (CVar-gated). */
        void DrawSkeletonDebug();

        /** Screen-space bone-name labels for every skeletal mesh (CVar-gated); called from DrawViewport. */
        void DrawSkeletonNameLabels(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        /** Skeleton-debug toggles; call inside a menu/popup. Backed by Editor.Debug.Skeleton*
         *  CVars so every surface stays in sync. */
        void DrawSkeletonDebugMenuItems();

        bool BeginViewportToolbarGroup(char const* GroupID, ImVec2 GroupSize, const ImVec2& Padding);
        void EndViewportToolbarGroup();

        /** Is this editor tool for editing assets? */
        NODISCARD virtual bool IsAssetEditorTool() const { return false; }
        
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
            return { FFixedString::CtorSprintf(), "%s##%08X", ToolWindowName, InDockspaceID };
        }

    public:

        /** Returns a transform placed in front of the active editor camera by the given distance. */
        FTransform GetCameraSpawnTransform(float DistanceForward = 5.0f) const;

        /** Dispatches a content-browser asset drop by asset class. Returns the spawned entity (or entt::null). */
        entt::entity HandleContentBrowserAssetDrop(FStringView VirtualPath, entt::entity DropTarget);

    protected:

        /** Undo/redo is only meaningful for an editable (Editor-type) world. Play/Simulate run on a
         *  transient PIE world that is discarded on stop, so transacting it is pointless and a full-registry
         *  serialize there causes a frame hitch (e.g. dragging the gizmo while simulating). */
        bool CanTransact() const { return World != nullptr && World->GetWorldType() == EWorldType::Editor; }

        /** Begin a transaction; captures before-state. */
        virtual void BeginTransaction();

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
        void RegisterAction(FEditorAction Action) { EditorActions.push_back(eastl::move(Action)); }

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
        entt::entity                        EditorEntity;
        FEditorCameraState                  CameraState;
        ImTextureID                         SceneViewportTexture = 0;

        TUniquePtr<FInputViewport>          InputViewport;

        EEditorToolFlags                    ToolFlags = EEditorToolFlags::Tool_WantsToolbar;

        bool                                bViewportFocused = false;
        bool                                bViewportHovered = false;
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
virtual char const* GetUniqueTypeName() const { return s_uniqueTypeName; }\
virtual uint32 GetUniqueTypeID() const override final { return TypeName::s_toolTypeID; }\
virtual bool IsSingleton() const override final { return true; }