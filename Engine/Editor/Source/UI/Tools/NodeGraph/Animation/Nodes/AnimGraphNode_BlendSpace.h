#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_BlendSpace.generated.h"

namespace Lumina
{
    class CBlendSpace;

    // Samples a blend space at the X/Y inputs. The node owns its playback phase, shared across every
    // contributing clip, so blends between clips of different lengths stay stride-aligned.
    REFLECT()
    class CAnimGraphNode_BlendSpace : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Blend Space"; }
        FStringView GetNodeTooltip() const override { return "Blends between animation clips placed in a 1D or 2D value space."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Blend space sampled by this node. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CBlendSpace> BlendSpace;

        /** Seconds for X to cover half the distance to its input. 0 feeds the raw value, which pops
         *  whenever the input jumps (a speed that steps from 0 to 600 on the frame the stick moves). */
        PROPERTY(Editable, Category = "Smoothing", ClampMin = 0.0f)
        float XSmoothingHalfLife = 0.0f;

        PROPERTY(Editable, Category = "Smoothing", ClampMin = 0.0f)
        float YSmoothingHalfLife = 0.0f;

        CAnimGraphPin* BlendSpacePin = nullptr;
        CAnimGraphPin* XPin = nullptr;
        CAnimGraphPin* YPin = nullptr;
        CAnimGraphPin* SpeedPin = nullptr;
        CAnimGraphPin* StartPositionPin = nullptr;
        CAnimGraphPin* PosePin = nullptr;
    };
}
