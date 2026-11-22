#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Blend.generated.h"

namespace Lumina
{
    // Blends two poses from A at alpha 0 to B at alpha 1, using slerp for rotations.
    REFLECT(MinimalAPI)
    class CAnimGraphNode_Blend : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Blend"; }
        FStringView GetNodeTooltip() const override { return "Blends pose A and pose B by a scalar alpha."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;
        float GetPoseInputShare(const CEdNodeGraphPin* PoseInput, const FAnimGraphDebugView& View) const override;

        CAnimGraphPin* PoseAPin = nullptr;
        CAnimGraphPin* PoseBPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
