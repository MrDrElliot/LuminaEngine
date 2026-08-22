#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_TwoBoneIK.generated.h"

namespace Lumina
{
    // Analytical two-bone IK: rotates Root + Mid so the tip (End) reaches a component-space
    // target while a Pole vector picks the bend side. For foot/hand placement, weapon grip, etc.
    REFLECT()
    class CAnimGraphNode_TwoBoneIK : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Two-Bone IK"; }
        FStringView GetNodeTooltip() const override { return "Analytical IK for a 3-joint chain (e.g. shoulder/elbow/wrist or hip/knee/ankle)."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Root joint (e.g. shoulder, hip). */
        PROPERTY(Editable, Category = "Chain", Picker = "Bone")
        FName RootBone;

        /** Mid joint between Root and End (e.g. elbow, knee). Its parent must be Root. */
        PROPERTY(Editable, Category = "Chain", Picker = "Bone")
        FName MidBone;

        /** End / tip joint (e.g. wrist, ankle). Its parent must be Mid. */
        PROPERTY(Editable, Category = "Chain", Picker = "Bone")
        FName EndBone;

        /** Component-space "pole" point that picks the bend side of the chain
         *  (e.g. in front of the elbow). The chain bends toward this point. */
        PROPERTY(Editable, Category = "Pole")
        FVector3 PoleVector = FVector3(0.0f, 0.0f, 1.0f);

        CAnimGraphPin* PoseInPin = nullptr;
        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* TargetXPin = nullptr;
        CAnimGraphPin* TargetYPin = nullptr;
        CAnimGraphPin* TargetZPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };
}
