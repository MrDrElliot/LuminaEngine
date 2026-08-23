#pragma once

#include "Animation/AnimationGraphVM.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Additive.generated.h"

namespace Lumina
{
    // Converts a pose into a delta against the Base pin, or against the bind pose when Base is unconnected.
    REFLECT()
    class CAnimGraphNode_MakeAdditive : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Make Additive"; }
        FStringView GetNodeTooltip() const override { return "Converts a pose into an additive delta against the Base pose, or against the skeleton's bind pose when Base is unconnected."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Local Space subtracts in each bone's parent frame; Mesh Space takes the rotation delta in component space. */
        PROPERTY(Editable, Category = "Additive")
        EAdditiveSpace Space = EAdditiveSpace::LocalSpace;

        CAnimGraphPin* PoseInputPin = nullptr;
        CAnimGraphPin* BasePosePin = nullptr;
        CAnimGraphPin* DeltaOutputPin = nullptr;
    };

    // Layers an additive delta on top of a base pose by an alpha. Alpha 0 yields
    // the base unchanged; alpha 1 yields the base with the full delta applied.
    REFLECT()
    class CAnimGraphNode_ApplyAdditive : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Apply Additive"; }
        FStringView GetNodeTooltip() const override { return "Adds a delta pose (produced by Make Additive or an additive clip) on top of a base pose. A mesh-space delta is applied in mesh space automatically."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* BasePin = nullptr;
        CAnimGraphPin* DeltaPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
