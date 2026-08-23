#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_IK.generated.h"

namespace Lumina
{
    // Iterative solver for a chain longer than three joints, where the analytical solver does not apply.
    REFLECT()
    class CAnimGraphNode_FABRIK : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "FABRIK"; }
        FStringView GetNodeTooltip() const override { return "Reaches a component-space target with a chain of any length, from Root Bone down to Tip Bone. Use it for spines, tails and tentacles; a three-joint limb is better served by Two-Bone IK, which is exact and cheaper."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Top of the chain. Tip Bone must descend from it. */
        PROPERTY(Editable, Category = "Chain", Picker = "Bone")
        FName RootBone;

        /** End of the chain, the joint driven onto the target. */
        PROPERTY(Editable, Category = "Chain", Picker = "Bone")
        FName TipBone;

        /** Solver passes per frame. More is closer to the target and costs more; 10 is plenty for a spine. */
        PROPERTY(Editable, Category = "Chain", ClampMin = 1, ClampMax = 32)
        int32 Iterations = 10;

        CAnimGraphPin* PoseInPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* TargetXPin = nullptr;
        CAnimGraphPin* TargetYPin = nullptr;
        CAnimGraphPin* TargetZPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };

    // Turns one bone toward a target, which is how a head or a gun tracks something.
    REFLECT()
    class CAnimGraphNode_LookAt : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Look At"; }
        FStringView GetNodeTooltip() const override { return "Rotates one bone so its Local Forward axis points at a component-space target, clamped to Max Angle from where the animation had it. Chain several with falloff alphas to spread a look across neck and spine."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Bone that turns. */
        PROPERTY(Editable, Category = "Look At", Picker = "Bone")
        FName Bone;

        /** Axis of the bone that should end up pointing at the target, in the bone's own space. */
        PROPERTY(Editable, Category = "Look At")
        FVector3 LocalForward = FVector3(0.0f, 0.0f, 1.0f);

        /** Degrees the bone may turn from its animated direction. 0 or less does not clamp. */
        PROPERTY(Editable, Category = "Look At", ClampMin = 0.0f, ClampMax = 180.0f, Units = "Degrees")
        float MaxAngle = 70.0f;

        CAnimGraphPin* PoseInPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* TargetXPin = nullptr;
        CAnimGraphPin* TargetYPin = nullptr;
        CAnimGraphPin* TargetZPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };

    // One leg planted on ground the game has already traced for.
    REFLECT()
    class CAnimGraphNode_FootIK : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Foot IK"; }
        FStringView GetNodeTooltip() const override { return "Moves one foot by Offset, solves the leg onto it, then rolls the foot onto Ground Normal. Offset and normal are component space and come from the game's own ground trace, since a solver running on a worker thread cannot query the scene. Pair two of these with a Translate Bone on the pelvis."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Upper leg. Its child must be the calf. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName ThighBone;

        /** Lower leg, parented to the thigh. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName CalfBone;

        /** Foot, parented to the calf. This is what lands on the offset target. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName FootBone;

        /** Axis of the foot bone that points away from the ground, in the foot's own space. */
        PROPERTY(Editable, Category = "Leg")
        FVector3 FootUpAxis = FVector3(0.0f, 1.0f, 0.0f);

        CAnimGraphPin* PoseInPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* OffsetXPin = nullptr;
        CAnimGraphPin* OffsetYPin = nullptr;
        CAnimGraphPin* OffsetZPin = nullptr;
        CAnimGraphPin* NormalXPin = nullptr;
        CAnimGraphPin* NormalYPin = nullptr;
        CAnimGraphPin* NormalZPin = nullptr;
        CAnimGraphPin* AlignPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };

    // Displaces a bone by a runtime value, which a baked Bone Transform cannot express.
    REFLECT()
    class CAnimGraphNode_TranslateBone : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Translate Bone"; }
        FStringView GetNodeTooltip() const override { return "Adds a component-space offset to one bone, leaving its rotation alone. Lowering the pelvis by however far the feet had to reach is what this is for, and unlike Bone Transform the offset is wired rather than baked."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Bone that moves. Its children follow, as they do for any pose edit. */
        PROPERTY(Editable, Category = "Bone", Picker = "Bone")
        FName Bone;

        CAnimGraphPin* PoseInPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* OffsetXPin = nullptr;
        CAnimGraphPin* OffsetYPin = nullptr;
        CAnimGraphPin* OffsetZPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };
}
