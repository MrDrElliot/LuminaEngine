#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Slot.generated.h"

namespace Lumina
{
    // Layers whatever montage is playing on the named slot over the Source pose.
    REFLECT()
    class CAnimGraphNode_Slot : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Slot"; }
        FStringView GetNodeTooltip() const override { return "Plays gameplay-driven montages on a named slot, over the incoming pose."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Name a montage's slot track must match to play here. */
        PROPERTY(Editable, Category = "Slot")
        FName SlotName = "DefaultSlot";

        CAnimGraphPin* SourcePin = nullptr;
        CAnimGraphPin* PosePin = nullptr;
    };
}
