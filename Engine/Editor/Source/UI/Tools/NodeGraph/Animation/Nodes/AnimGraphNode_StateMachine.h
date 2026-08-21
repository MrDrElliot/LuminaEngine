#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_StateMachine.generated.h"

namespace Lumina
{
    class CAnimStateMachineGraph;

    // A state machine inside a blend-tree graph (double-click for its own canvas of State
    // nodes + transition wires); outputs the resolved pose of whichever state is active.
    REFLECT()
    class CAnimGraphNode_StateMachine : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "State Machine"; }
        FStringView GetNodeTooltip() const override { return "A state machine. Double-click to edit its states and transitions."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(120, 70, 150, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FString GetNodeTitleText() const override;
        bool GetRenameText(FString& OutText) const override;
        void SetRenameText(const FString& InText) override;

        // Double-click descends into the state machine canvas, creating it on
        // first access.
        CEdNodeGraph* GetEnterableSubGraph() override;

        // Returns (creating if needed) the state machine canvas graph.
        CAnimStateMachineGraph* GetOrCreateStateMachineGraph();

        // Without this a duplicated machine shares the original's canvas and edits track both.
        void PostCloneFrom(const CEdGraphNode* Source) override;

        CEdNodeGraph* GetOwnedSubGraph() const override;
        void ReplaceSubGraphWithCopy() override;

        // Fresh and un-set-up, so a caller can clone into it before EnsureSetup adds an Entry node.
        CAnimStateMachineGraph* AllocateStateMachineGraph();

        /** Name shown on the node and in the breadcrumb bar. Press F2 on the node to change it. */
        PROPERTY(Editable, Category = "State Machine")
        FName MachineName;

        /** The state machine's canvas. Allocated lazily; edited by double-clicking the node. */
        PROPERTY()
        TObjectPtr<CAnimStateMachineGraph> StateMachineGraph;

        CAnimGraphPin* ResultPin = nullptr;
    };
}
