#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_ClipEvaluator.generated.h"

namespace Lumina
{
    class CAnimation;

    // Samples a clip at an explicit time, for poses indexed by a value rather than played on a clock.
    REFLECT()
    class CAnimGraphNode_ClipEvaluator : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Evaluate Animation Clip"; }
        FStringView GetNodeTooltip() const override { return "Samples a clip at the Time input instead of on a playback clock, which is how a pose is indexed by a value (a lean scrubbed by angle, a turn indexed by yaw). Normalized treats Time as 0..1 across the clip; otherwise it is seconds."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Clip sampled by this node. A wired Animation pin overrides it at runtime. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CAnimation> Clip;

        /** On, Time is a 0..1 fraction of the clip; off, it is seconds. */
        PROPERTY(Editable, Category = "Animation")
        bool bNormalizedTime = true;

        CAnimGraphPin* AnimationPin = nullptr;
        CAnimGraphPin* TimePin = nullptr;
        CAnimGraphPin* PosePin = nullptr;
    };
}
