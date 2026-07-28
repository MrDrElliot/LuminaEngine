#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Additive.generated.h"

namespace Lumina
{
    // Converts a regular pose into an additive delta relative to the skeleton's bind pose.
    // Pair with Apply Additive to layer expressions/lean/look-at on top of a base pose.
    REFLECT()
    class CAnimGraphNode_MakeAdditive : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Make Additive"; }
        FStringView GetNodeTooltip() const override { return "Converts a pose into an additive delta relative to the skeleton's bind pose."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* PoseInputPin = nullptr;
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
        FStringView GetNodeTooltip() const override { return "Adds a delta pose (produced by Make Additive or an additive clip) on top of a base pose."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        CAnimGraphPin* BasePin = nullptr;
        CAnimGraphPin* DeltaPin = nullptr;
        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
