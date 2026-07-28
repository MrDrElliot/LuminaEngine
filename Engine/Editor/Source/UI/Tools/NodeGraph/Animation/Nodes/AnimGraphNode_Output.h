#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Output.generated.h"

namespace Lumina
{
    // Terminal node: the pose wired into its single input is the frame's final pose.
    // Exactly one exists per graph and it cannot be deleted.
    REFLECT()
    class CAnimGraphNode_Output : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Output Pose"; }
        FStringView GetNodeTooltip() const override { return "Final pose evaluated by the animation graph."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(35, 35, 200, 255); }
        bool IsDeletable() const override { return false; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        // Pose register resolved from the Result Pose input during the last compile; the owning
        // graph emits the final Output opcode (top-level) or threads it into a state machine (sub-graph).
        uint16 GetResolvedPoseRegister() const { return ResolvedPoseRegister; }

        CAnimGraphPin* ResultPosePin = nullptr;

    private:

        uint16 ResolvedPoseRegister = 0;
    };
}
