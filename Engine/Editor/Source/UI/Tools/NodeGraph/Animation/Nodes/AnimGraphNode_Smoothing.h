#pragma once

#include "Animation/AnimationGraphVM.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Smoothing.generated.h"

namespace Lumina
{
    // Smooths a discontinuity in its input pose by decaying the offset from the last shown pose.
    REFLECT()
    class CAnimGraphNode_Inertialization : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Inertialization"; }
        FStringView GetNodeTooltip() const override { return "Smooths a jump in the incoming pose. On the rising edge of Request it captures the offset from the pose shown last frame, then decays that offset to zero over Duration, so the switch is C1 continuous with no second pose evaluated."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* PoseInputPin = nullptr;
        CAnimGraphPin* RequestPin = nullptr;
        CAnimGraphPin* DurationPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };

    // Same seam, extrapolated instead of offset, which suits a source that was moving fast.
    REFLECT()
    class CAnimGraphNode_DeadBlending : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Dead Blending"; }
        FStringView GetNodeTooltip() const override { return "Smooths a jump in the incoming pose by extrapolating the pose shown last frame from its own velocity, decaying that motion over Half Life, and cross-fading it into the new pose over Duration. Reads better than Inertialization when the old pose was moving fast."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* PoseInputPin = nullptr;
        CAnimGraphPin* RequestPin = nullptr;
        CAnimGraphPin* DurationPin = nullptr;
        CAnimGraphPin* HalfLifePin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
