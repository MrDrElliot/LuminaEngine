#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationGraphNodeGraph.generated.h"

namespace Lumina
{
    class CAnimationGraph;
    class FAnimationGraphCompiler;

    // Editor-side node graph for animation graphs (mirrors CMaterialNodeGraph): CompileGraph walks the
    // authored nodes in topological order, emitting bytecode into the runtime CAnimationGraph asset.
    REFLECT()
    class CAnimationGraphNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()
    public:

        void Initialize() override;
        void Shutdown() override;

        // Context-free setup (no node-editor context needed): ensures an Output node and registers
        // creatable types, so the compiler can ready a never-opened state blend tree. Idempotent.
        void EnsureSetup();

        // Rebuilds Connections from live pin state after every connection edit; without the override
        // the array stays empty and links are lost on save (mirrors the material graph).
        void ValidateGraph() override;

        const FEdGraphSchema& GetSchema() const override;

        // Animation nodes expose value/enum defaults inline on their input pins.
        bool ShouldDrawInlinePinEditors() const override { return true; }

        // Topo-sorts from the Output node and emits bytecode for each contributing node, finishing with
        // an Output opcode. Errors attach to their nodes. Top-level graph entry point.
        void CompileGraph(FAnimationGraphCompiler& Compiler);

        // Compiles into the shared register space WITHOUT a final Output opcode, returning the Output node's
        // feeding pose register in OutPoseReg (used for a state's blend tree). Returns false on cycle / missing Output.
        bool CompileNodes(FAnimationGraphCompiler& Compiler, uint16& OutPoseReg);

        // Registers every parameter declared in this tree, including from nodes the topo-sort skips because
        // they aren't wired into the Output. Recurses into nested state machines / per-state blend trees.
        void CollectAllParameters(FAnimationGraphCompiler& Compiler);

        void SetAnimationGraph(CAnimationGraph* InGraph) { AnimationGraph = InGraph; }
        CAnimationGraph* GetAnimationGraph() const { return AnimationGraph; }

    private:

        // Cache names read by Use Cached Pose nodes under these, nested state blend trees included.
        static void CollectReadCacheNames(const THashSet<CEdGraphNode*>& SubtreeNodes, THashSet<FName>& OutNames);
        static void CollectReadCacheNamesInGraph(CAnimationGraphNodeGraph* Graph, THashSet<FName>& OutNames);

        TObjectPtr<CAnimationGraph> AnimationGraph;

        // One-shot guards; not serialized. bSetupDone covers context-free setup; bInitialized covers
        // full Initialize() (which additionally creates the node-editor context).
        bool bSetupDone = false;
        bool bInitialized = false;
    };
}
