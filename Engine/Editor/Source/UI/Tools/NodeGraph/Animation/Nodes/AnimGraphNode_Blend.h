#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Blend.generated.h"

namespace Lumina
{
    // Linearly blends two poses. Alpha 0 yields pose A, alpha 1 yields pose B;
    // rotations are blended with slerp so the result stays valid.
    REFLECT()
    class CAnimGraphNode_Blend : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Blend"; }
        FStringView GetNodeTooltip() const override { return "Blends pose A and pose B by a scalar alpha."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* PoseAPin = nullptr;
        CAnimGraphPin* PoseBPin = nullptr;
        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
