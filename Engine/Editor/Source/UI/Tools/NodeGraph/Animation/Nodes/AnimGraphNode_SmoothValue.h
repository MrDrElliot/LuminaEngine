#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_SmoothValue.generated.h"

namespace Lumina
{
    // Eases a value toward its input, so a stepped gameplay value does not pop whatever it drives.
    REFLECT()
    class CAnimGraphNode_SmoothValue : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Smooth Value"; }
        FStringView GetNodeTooltip() const override { return "Exponentially eases the input toward its target. Half Life is the seconds taken to cover half the remaining distance, so it is frame-rate independent. 0 passes the value straight through, and the first frame always snaps."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* ValuePin = nullptr;
        CAnimGraphPin* HalfLifePin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
