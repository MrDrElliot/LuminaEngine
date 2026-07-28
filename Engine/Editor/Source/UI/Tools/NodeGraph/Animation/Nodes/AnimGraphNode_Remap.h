#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Remap.generated.h"

namespace Lumina
{
    // Remaps a scalar from an input range to an output range, built from existing scalar opcodes (no
    // dedicated VM instruction). Turns a raw parameter (speed, distance) into a normalized blend alpha.
    REFLECT()
    class CAnimGraphNode_Remap : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Remap"; }
        FStringView GetNodeTooltip() const override { return "Remaps a value from [In Min, In Max] to [Out Min, Out Max]."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** When true, the normalized input is clamped to [0, 1] before scaling. */
        PROPERTY(Editable, Category = "Remap")
        bool bClampToRange = true;

        CAnimGraphPin* ValuePin = nullptr;
        CAnimGraphPin* InMinPin = nullptr;
        CAnimGraphPin* InMaxPin = nullptr;
        CAnimGraphPin* OutMinPin = nullptr;
        CAnimGraphPin* OutMaxPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
