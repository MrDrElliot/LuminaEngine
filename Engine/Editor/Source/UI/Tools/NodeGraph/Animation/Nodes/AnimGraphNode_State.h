#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_State.generated.h"

namespace Lumina
{
    class CAnimationGraphNodeGraph;

    // A state on the state machine canvas that owns its blend-tree graph and links to others by StateFlow wires.
    REFLECT()
    class CAnimGraphNode_State : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        // Lives on the state machine canvas, not the blend tree, so it narrows the anim-node default.
        CClass* GetSupportedGraphClass() const override;

        FStringView GetNodeDisplayName() const override { return "State"; }
        FStringView GetNodeTooltip() const override { return "A named animation state. Double-click to edit its blend tree."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(95, 80, 150, 255); }

        void BuildNode() override;

        FString GetNodeTitleText() const override;
        bool GetRenameText(FString& OutText) const override;
        void SetRenameText(const FString& InText) override;

        // The blend-tree compiler's topological walk never visits state nodes, so this is a no-op.
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        // Double-click descends into the blend tree, creating it on first access.
        CEdNodeGraph* GetEnterableSubGraph() override;

        // Creates the blend tree if needed, for both the editor and the compiler.
        CAnimationGraphNodeGraph* GetOrCreateBlendTree();

        // Without this a duplicated state shares the original's blend tree and edits track both.
        void PostCloneFrom(const CEdGraphNode* Source) override;

        CEdNodeGraph* GetOwnedSubGraph() const override;
        void ReplaceSubGraphWithCopy() override;

        // Fresh and un-set-up, so a caller can clone into it before EnsureSetup adds an Output node.
        CAnimationGraphNodeGraph* AllocateBlendTree();

        /** Name used to reference this state from the entry connection and transitions. */
        PROPERTY(Editable, Category = "State")
        FName StateName;

        /** This state's blend tree. Allocated lazily; edited by double-clicking the node. */
        PROPERTY(Hidden)
        TStrongObjectPtr<CAnimationGraphNodeGraph> BlendTree;

        // StateName, or a placeholder when unnamed. What the canvas and transition lists label it with.
        FString GetStateLabel() const;

        CAnimGraphPin* InPin = nullptr;
        CAnimGraphPin* OutPin = nullptr;
    };

    // The single entry point of a state machine canvas, created by the graph itself and not deletable.
    REFLECT(NotPlaceable)
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

    // Source of transitions checked from any active state, compiled as FromState -1.
    REFLECT()
    class CAnimGraphNode_StateAny : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        // Lives on the state machine canvas, not the blend tree, so it narrows the anim-node default.
        CClass* GetSupportedGraphClass() const override;

        FStringView GetNodeDisplayName() const override { return "Any State"; }
        FStringView GetNodeTooltip() const override { return "Transitions out of this are checked from every state, not just one."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(150, 110, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        CAnimGraphPin* OutPin = nullptr;
    };
}
