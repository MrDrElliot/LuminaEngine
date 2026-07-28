#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_State.generated.h"

namespace Lumina
{
    class CAnimationGraphNodeGraph;

    // A state on the state machine canvas, owning its blend-tree graph (double-click to edit) and connected
    // to other states via StateFlow transition wires. The State Machine node compiles it to a pose register.
    REFLECT()
    class CAnimGraphNode_State : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "State"; }
        FStringView GetNodeTooltip() const override { return "A named animation state. Double-click to edit its blend tree."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(95, 80, 150, 255); }

        void BuildNode() override;

        // State nodes live on the state machine canvas; they are never visited
        // by the blend-tree compiler's topological walk, so this is a no-op.
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        // Double-click descends into this state's blend tree, creating it on
        // first access.
        CEdNodeGraph* GetEnterableSubGraph() override;

        // Returns (creating if needed) this state's blend-tree graph. Used by
        // both the editor (to draw it) and the compiler (to evaluate the state).
        CAnimationGraphNodeGraph* GetOrCreateBlendTree();

        /** Name used to reference this state from the entry connection and transitions. */
        PROPERTY(Editable, Category = "State")
        FName StateName;

        /** This state's blend tree. Allocated lazily; edited by double-clicking the node. */
        PROPERTY()
        TObjectPtr<CAnimationGraphNodeGraph> BlendTree;

        CAnimGraphPin* InPin = nullptr;
        CAnimGraphPin* OutPin = nullptr;
    };

    // The single entry point of a state machine canvas. Whichever State its
    // Entry output wires to is the state the machine starts in. Not deletable.
    REFLECT()
    class CAnimGraphNode_StateEntry : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Entry"; }
        FStringView GetNodeTooltip() const override { return "The state the machine starts in. Wire it to one State."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(60, 130, 90, 255); }
        bool IsDeletable() const override { return false; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        CAnimGraphPin* OutPin = nullptr;
    };
}
