#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_FloatConstant.generated.h"

namespace Lumina
{
    // Emits a constant scalar value. Useful as a fixed blend alpha or speed
    // multiplier when no parameter drive is needed.
    REFLECT()
    class CAnimGraphNode_FloatConstant : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Float Constant"; }
        FStringView GetNodeTooltip() const override { return "Emits a constant scalar value."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Constant value emitted on the Value output. */
        PROPERTY(Editable, Category = "Value")
        float Value = 0.0f;

        CAnimGraphPin* ValuePin = nullptr;
    };
}
