#pragma once

#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/AssetEditors/Animation/AnimPreview.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphDebugView.h"

namespace Lumina
{
    class CAnimation;
    class CAnimationGraph;
    class CAnimationGraphNodeGraph;
    class CAnimGraphNode_State;
    class CAnimStateMachineGraph;
    class CAnimStateTransition;
    class CBlendSpace;
    class CEdGraphNode;
    class CEdNodeGraph;
    class CEdNodeGraphPin;
    class CWorld;
    struct FAnimGraphVMState;

    class FAnimationGraphEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAnimationGraphEditorTool)

        FAnimationGraphEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_RUN_FAST; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnPostUndoRedo() override;
        void OnSave() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        CEdNodeGraph* GetActiveNodeGraph() override;
        void AdoptNodeGraph(CEdNodeGraph* Graph) override { EnsureGraphReady(Graph); }

    private:

        struct FGraphStackEntry
        {
            CEdNodeGraph*   Graph = nullptr;
            FString         Label;
        };

        struct FClipEntry
        {
            FString         Path;
            FString         DisplayName;
        };

        void EnsureGraphReady(CEdNodeGraph* Graph);
        void WireGraphCallbacks(CEdNodeGraph* Graph);
        void EnterGraph(CEdNodeGraph* Graph, const FString& Label);
        void PopToLevel(int32 Index);
        void NavigateBack();
        void NavigateForward();
        void ClearGraphSelection();
        void SyncPreviewMesh();

        // The per-frame preview recompile skips dirtying the package, so the asset is not always unsaved.
        void Compile(bool bMarkPackageDirty = true);
        NODISCARD bool NeedsCompile() const;

        void DrawGraphWindow();
        void DrawBreadcrumbBar();
        void DrawPropertiesWindow();
        void DrawPreviewControls();
        void DrawOutgoingTransitionsForState(CAnimGraphNode_State* State);
        void DrawTransitionBlend(CAnimStateTransition* Transition);
        void DrawParametersWindow();
        void DrawParameters(CAnimationGraph* Graph);
        void DrawLiveCurveValues(CAnimationGraph* Graph);
        void DrawClipBrowserWindow();
        void DrawTaskGraphWindow();
        void DrawDebugTargetCombo();

        NODISCARD bool ResolveDebugTarget(CWorld*& OutWorld, ECS::FEntity& OutEntity) const;

        void UpdateDebugOverlay();
        void UpdateDebugTransition(CAnimStateMachineGraph* SMGraph, const FAnimGraphVMState& VMState);
        void UpdateDebugView(const FAnimGraphVMState* VMState);
        void UpdateMachineStatuses(CEdNodeGraph* Graph, const FAnimGraphVMState& VMState);

        // A state's blend tree supplies the result only while that state, and every state above it, is current.
        NODISCARD float ResolveDisplayedRootWeight() const;

        void SpawnClipPlayerNode(CAnimation* Clip, ImVec2 ScreenPos);
        void SpawnBlendSpaceNode(CBlendSpace* BlendSpace, ImVec2 ScreenPos);
        void WireAxisParameter(FName AxisName, CEdNodeGraphPin* AxisPin, ImVec2 ScreenPos);
        void FinishSpawnedNode(CEdGraphNode* Node, ImVec2 ScreenPos);

        TStrongObjectPtr<CAnimationGraphNodeGraph>                      NodeGraph;
        CEdGraphNode*                                                   SelectedNode = nullptr;
        CAnimStateTransition*                                           SelectedTransition = nullptr;

        // GraphStack[0] is the top level and back() is the drawn canvas; forward holds levels stepped out of, newest last.
        TVector<FGraphStackEntry>                                       GraphStack;
        TVector<FGraphStackEntry>                                       NavForwardStack;
        THashSet<CEdNodeGraph*>                                         InitializedGraphs;

        FString                                                         CompilationLog;
        bool                                                            bHasCompilationErrors = false;
        bool                                                            bAutoCompile = true;

        FPropertyEditContext                                            PropertyContext;
        FSkeletonEditContext                                            SkeletonCtx;
        FAnimGraphEditContext                                           AnimGraphCtx;
        FAnimStateMachineEditContext                                    StateMachineCtx;
        THashMap<CAnimStateTransition*, TUniquePtr<FPropertyTable>>     TransitionTables;
        TUniquePtr<FPropertyTable>                                      ParameterTable;

        bool                                                            bDebugEnabled = false;

        // A null world means the editor preview.
        TWeakObjectPtr<CWorld>                                          DebugTargetWorld;
        ECS::FEntity                                                    DebugTargetEntity = ECS::NullEntity;

        FAnimGraphDebugView                                             DebugView;
        TVector<FAnimGraphDebugStateNode>                               DebugStateNodes;
        THashMap<FName, FAnimValueHistory>                              CurveHistories;
        CAnimStateTransition*                                           DebugTransition = nullptr;
        float                                                           DebugTransitionWeight = 0.0f;

        // Members so the pointers handed to FGraphDebugContext stay valid through the draw.
        THashMap<CEdNodeGraphPin*, FString>                             DebugPinValues;
        THashSet<const CEdGraphNode*>                                   DebugActiveNodes;

        TVector<FClipEntry>                                             ClipEntries;
        const CObject*                                                  ClipCacheSkeleton = nullptr;
        FDelegateHandle                                                 RegistryUpdatedHandle;
        ImGuiTextFilter                                                 ClipFilter;
        bool                                                            bClipCacheStale = true;
        bool                                                            bClipBrowserAnySkeleton = false;

        FPreviewMeshSelection                                           PreviewMeshSelection;

        // Where a double-clicked clip lands, since a double-click has no cursor position on the canvas.
        ImVec2                                                          GraphCanvasCenter = ImVec2(0.0f, 0.0f);

        FAnimTaskSnapshot                                               TaskSnapshot;
        float                                                           TaskGraphZoom = 1.0f;
        bool                                                            bTaskGraphShowSkipped = true;

        ECS::FEntity                                                    MeshEntity = ECS::NullEntity;
        ECS::FEntity                                                    DirectionalLightEntity = ECS::NullEntity;
    };
}
