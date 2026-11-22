#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Output.generated.h"

namespace Lumina
{
    // Terminal node whose input is the final pose, created by the graph itself so it is never in the palette.
    REFLECT(NotPlaceable, MinimalAPI)
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

        // Resolved during the last compile, and the owning graph emits the Output opcode or threads it into a state machine.
        uint16 GetResolvedPoseRegister() const { return ResolvedPoseRegister; }

        CAnimGraphPin* ResultPosePin = nullptr;

    private:

        uint16 ResolvedPoseRegister = 0;
    };
}
