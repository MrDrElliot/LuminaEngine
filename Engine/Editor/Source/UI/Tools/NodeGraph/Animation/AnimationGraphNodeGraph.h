#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Containers/Optional.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationGraphNodeGraph.generated.h"

namespace Lumina
{
    class CAnimationGraph;
    class FAnimationGraphCompiler;
    struct FAnimGraphDebugView;

    // Editor-side animation node graph, compiled in topological order into the runtime CAnimationGraph.
    REFLECT()
    class EDITOR_API CAnimationGraphNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()
    public:

        void Initialize() override;
        void Shutdown() override;

        // Idempotent setup that needs no node-editor context, so the compiler can ready a never-opened blend tree.
        void EnsureSetup();

        // Without this override Connections stays empty and links are lost on save.
        void ValidateGraph() override;

        const FEdGraphSchema& GetSchema() const override;

        // Animation nodes expose value/enum defaults inline on their input pins.
        bool ShouldDrawInlinePinEditors() const override { return true; }

        // Top-level entry point that finishes with an Output opcode, with errors attached to their nodes.
        void CompileGraph(FAnimationGraphCompiler& Compiler);

        // Compiles without a final Output opcode, as a state's blend tree needs. Returns false on a cycle or missing Output.
        bool CompileNodes(FAnimationGraphCompiler& Compiler, uint16& OutPoseReg);

        // Includes nodes the topo-sort skips for not being wired into the Output, and recurses into nested machines.
        void CollectAllParameters(FAnimationGraphCompiler& Compiler);

        void SetAnimationGraph(CAnimationGraph* InGraph) { AnimationGraph = InGraph; }
        CAnimationGraph* GetAnimationGraph() const { return AnimationGraph.Get(); }

        // Borrowed from the editor tool that runs the graph, and cleared before that tool goes.
        void SetDebugView(const FAnimGraphDebugView* InView) { DebugView = InView; }

        // How much of the result each pose node supplies, walking back from the Output node, which supplies RootWeight.
        void ComputePoseWeights(FAnimGraphDebugView& View, float RootWeight) const;

        bool HasNodePreview(const CEdGraphNode* Node) override;
        void DrawNodePreviewContent(CEdGraphNode* Node) override;
        bool IsNodeIdle(const CEdGraphNode* Node) const override;
        void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const override;
        bool GetLinkBadge(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, FString& OutText, ImU32& OutColor) override;
        bool WantsLinkFlow(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin) override;

    private:

        // The share of the result a pose wire carries, unset for a value wire or while weights are hidden.
        TOptional<float> GetPoseLinkWeight(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin) const;

        // Cache names read by Use Cached Pose nodes under these, nested state blend trees included.
        static void CollectReadCacheNames(const THashSet<CEdGraphNode*>& SubtreeNodes, THashSet<FName>& OutNames);
        static void CollectReadCacheNamesInGraph(CAnimationGraphNodeGraph* Graph, THashSet<FName>& OutNames);

        TStrongObjectPtr<CAnimationGraph> AnimationGraph;

        // One-shot guards that are not serialized, with bInitialized also covering the node-editor context.
        bool bSetupDone = false;
        bool bInitialized = false;

        const FAnimGraphDebugView* DebugView = nullptr;
    };
}
