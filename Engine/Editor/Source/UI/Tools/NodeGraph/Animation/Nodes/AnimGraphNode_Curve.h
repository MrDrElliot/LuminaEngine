#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_Curve.generated.h"

namespace Lumina
{
    // Reads a named animation curve off an incoming pose. Curves are authored on clips and blended
    // through the graph with the pose, so the value reflects whatever the branch weights did to it.
    REFLECT()
    class CAnimGraphNode_GetCurve : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Get Curve"; }
        FStringView GetNodeTooltip() const override { return "Reads a named animation curve carried by the incoming pose."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Curve to read; must be authored on one of the graph's clips or written by a Set Curve node. */
        PROPERTY(Editable, Category = "Curve", Picker = "Curve")
        FName CurveName;

        CAnimGraphPin* PosePin = nullptr;
        CAnimGraphPin* ValuePin = nullptr;
    };

    // Overrides a named curve on the incoming pose. The pose itself passes through untouched.
    REFLECT()
    class CAnimGraphNode_SetCurve : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Set Curve"; }
        FStringView GetNodeTooltip() const override { return "Writes a named animation curve onto the pose, replacing whatever the clips produced."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Curve to write. Downstream Get Curve nodes and script reads see this value. */
        PROPERTY(Editable, Category = "Curve", Picker = "Curve")
        FName CurveName;

        CAnimGraphPin* PosePin = nullptr;
        CAnimGraphPin* ValuePin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
