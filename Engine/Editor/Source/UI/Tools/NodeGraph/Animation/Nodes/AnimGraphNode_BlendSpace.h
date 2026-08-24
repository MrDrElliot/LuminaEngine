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

        CAnimGraphPin* BlendSpacePin = nullptr;
        CAnimGraphPin* XPin = nullptr;
        CAnimGraphPin* YPin = nullptr;
        CAnimGraphPin* SpeedPin = nullptr;
        CAnimGraphPin* StartPositionPin = nullptr;
        CAnimGraphPin* PosePin = nullptr;
    };
}
