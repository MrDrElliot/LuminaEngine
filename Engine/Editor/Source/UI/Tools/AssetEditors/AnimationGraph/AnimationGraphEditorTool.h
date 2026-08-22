#pragma once

#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    class CAnimationGraphNodeGraph;
    class CAnimStateTransition;
    class CEdNodeGraph;
    class CEdNodeGraphPin;
    class CEnum;
    class CWorld;

    // Asset editor for CAnimationGraph: node graph canvas, a 3D preview running the compiled
    // graph on the skeleton's preview mesh, and a properties panel. Compiles to bytecode on save.
    class FAnimationGraphEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAnimationGraphEditorTool)

        FAnimationGraphEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_RUN_FAST; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;

        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;

        // Lazily creates/updates/removes the preview mesh to track the graph's skeleton.
        // Runs every frame since the skeleton is often assigned after the tool opens.
        void SyncPreviewMesh();

        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return false; }

        void OnSave() override;

        void DrawGraphWindow();
        void DrawPropertiesWindow();
        void DrawPreviewControls();

        // Lists the compiled graph's parameters with editable live values, pushed into the
        // preview mesh so transition conditions and Get Parameter nodes can be exercised.
        void DrawParametersWindow();
        void DrawParameters(class CAnimationGraph* Graph);

        // Read-only live values of the curves the graph's clips carry, sampled off the debug target's
        // output pose. Curves are produced by evaluation, so unlike parameters they can't be driven here.
        void DrawLiveCurveValues(class CAnimationGraph* Graph);

        // Resolves a reflected enum by registered name (lazy cache), for the
        // Parameters panel's enum-key value combos.
        CEnum* ResolveReflectedEnum(const FName& Name);

        // When a State node is selected, lists its outgoing transitions inline so conditions
        // can be edited without clicking the (often hard to hit) transition wire.
        void DrawOutgoingTransitionsForState(class CAnimGraphNode_State* State);

        // Draws the navigation breadcrumb (Animation Graph > State Machine >
        // State ...) above the canvas; clicking a crumb pops back to that level.
        void DrawBreadcrumbBar();

        // Topologically compiles the node graph into the asset's bytecode. bMarkPackageDirty
        // is skipped for the per-frame preview recompile so the asset isn't always unsaved.
        void Compile(bool bMarkPackageDirty = true);

        // Content counter the auto-compile gate compares, covering the top graph and every nested canvas.
        uint64 GetContentVersion() const;

        // Gates the auto-compile, so a graph nobody is editing is not recompiled every frame.
        bool NeedsCompile() const;

        // Builds the live debug overlay (pin values, active state) from the target's VM state
        // and pushes it onto the displayed graph. No-op when the Debug toggle is off.
        void UpdateDebugOverlay();

        // Resolves the machine's live From/Current pair onto a transition and its blend weight.
        void UpdateDebugTransition(class CAnimStateMachineGraph* SMGraph, const struct FAnimGraphVMState& VMState);

        // Dropdown choosing the debug overlay's source: the editor preview, or any live
        // entity (across worlds) whose anim component uses this graph asset.
        void DrawDebugTargetCombo();

        // Visualizes the pose task list the debug target's graph compiled into this frame: the
        // dependency DAG laid out by level, what executed vs. what was skipped, and pose-buffer
        // ownership. Arms the runtime capture while the window is open.
        void DrawTaskGraphWindow();

        // Browser of animation clips authored against this graph's skeleton. Rows drag onto the
        // canvas (or double-click) to spawn a Play Animation Clip node with the clip assigned.
        void DrawClipBrowserWindow();

        // Spawns a Play Animation Clip node for Clip on the active canvas, positioned at ScreenPos.
        void SpawnClipPlayerNode(CAnimation* Clip, ImVec2 ScreenPos);

    private:

        // Resolves the debug target (dropdown selection, or the editor preview when unset) to a
        // live world + entity. False when neither is valid.
        bool ResolveDebugTarget(CWorld*& OutWorld, entt::entity& OutEntity) const;

        // One level of the graph navigation stack: the graph being drawn and the
        // label shown for it in the breadcrumb bar.
        struct FGraphStackEntry
        {
            CEdNodeGraph* Graph = nullptr;
            FString       Label;
        };

        // Descends into a sub-graph (state machine canvas, or a state's blend
        // tree): readies it, pushes it on the stack, resets the inspector.
        void EnterGraph(CEdNodeGraph* Graph, const FString& Label);

        // Pops the navigation stack back to the given level.
        void PopToLevel(int32 Index);

        // Thumb-button history over the same stack: back steps out one level, forward retraces.
        void NavigateBack();
        void NavigateForward();

        // Drops whatever the inspector was showing, since it belongs to the canvas being left.
        void ClearGraphSelection();

        // Initializes a graph (creates its node-editor context) and wires its
        // selection / double-click callbacks. Idempotent per graph.
        void EnsureGraphReady(CEdNodeGraph* Graph);
        void WireGraphCallbacks(CEdNodeGraph* Graph);

        TObjectPtr<CAnimationGraphNodeGraph>    NodeGraph;
        CEdGraphNode*                           SelectedNode = nullptr;
        CAnimStateTransition*                   SelectedTransition = nullptr;
        FString                                 CompilationLog;
        bool                                    bHasCompilationErrors = false;
        uint64                                  CompiledContentVersion = 0;
        bool                                    bHasCompiledOnce = false;

        // Graph navigation: GraphStack[0] is top-level, back() is the drawn canvas.
        // InitializedGraphs tracks graphs this tool created so they shut down on close.
        TVector<FGraphStackEntry>               GraphStack;

        // Levels stepped out of, newest last. Descending somewhere new discards them.
        TVector<FGraphStackEntry>               NavForwardStack;
        THashSet<CEdNodeGraph*>                 InitializedGraphs;

        // Editor-driven live parameter values, pushed into the preview mesh each frame; keyed
        // by name so they survive recompiles/reordering.

        // Lazily-built name -> reflected enum lookup for enum-key value combos.
        THashMap<FName, CEnum*>                 ReflectedEnumCache;
        bool                                    bEnumCacheBuilt = false;

        // Lazily-built inline property tables for the selected State's outgoing transitions,
        // keyed by transition ptr; cleared when the canvas's transition list changes.
        THashMap<CAnimStateTransition*, TUniquePtr<FPropertyTable>> TransitionTables;

        FPropertyEditContext                    PropertyContext;
        FSkeletonEditContext                    SkeletonCtx;
        FAnimGraphEditContext                   AnimGraphCtx;
        FAnimStateMachineEditContext            StateMachineCtx;

        // Read-only view of the preview entity's live parameter block; rebound when that memory moves.
        TUniquePtr<FPropertyTable>              ParameterTable;

        // Live preview: recompile whenever the graph changes, so edits need no manual compile.
        bool                                    bAutoCompile = true;

        // Debug overlay: when on, the graph animates link flow, prints live pin
        // values, and highlights the active state machine state.
        bool                                    bDebugEnabled = false;

        // Which instance the debug overlay reads from. Null world = the editor
        // preview (MeshEntity); otherwise a live entity in another world.
        TWeakObjectPtr<CWorld>                  DebugTargetWorld;
        entt::entity                            DebugTargetEntity = entt::null;

        // Captured per compile: maps editor pins to VM registers and State nodes to runtime
        // slots. Refreshed every frame by auto-compile, so pin pointers stay valid.
        THashMap<const CEdNodeGraphPin*, uint16> DebugPinRegisters;
        TVector<FAnimGraphDebugStateNode>        DebugStateNodes;

        // Per-frame overlay payload handed to the displayed graph; kept alive as
        // members so the pointers in FGraphDebugContext stay valid during draw.
        THashMap<CEdNodeGraphPin*, FString>      DebugPinValues;
        THashSet<const CEdGraphNode*>            DebugActiveNodes;

        // Clip browser: asset paths of clips matching the graph's skeleton, rebuilt when the
        // skeleton changes or the registry reports a different asset count.
        struct FClipEntry
        {
            FString Path;
            FString DisplayName;
        };
        TVector<FClipEntry>                     ClipEntries;
        const CObject*                          ClipCacheSkeleton = nullptr;
        int32                                   ClipCacheAssetCount = -1;
        ImGuiTextFilter                         ClipFilter;
        bool                                    bClipBrowserAnySkeleton = false;

        // Screen-space center of the graph canvas, refreshed each draw; double-clicking a clip in
        // the browser drops its node here (there's no cursor position to spawn at).
        ImVec2                                  GraphCanvasCenter = ImVec2(0.0f, 0.0f);

        // Last task list captured from the debug target, refreshed each frame the Task Graph
        // window is visible. Copied out of the runtime under its capture lock.
        FAnimTaskSnapshot                       TaskSnapshot;
        float                                   TaskGraphZoom = 1.0f;
        bool                                    bTaskGraphShowSkipped = true;

        entt::entity                            MeshEntity = entt::null;
        entt::entity                            DirectionalLightEntity = entt::null;
    };
}
