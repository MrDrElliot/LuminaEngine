#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "AnimGraphNode_BoneTransform.generated.h"

namespace Lumina
{
    // Procedural per-bone transform: drops a (Translation, Rotation, Scale) onto a named bone in its
    // local or component frame. For hand-IK style overrides, aim offsets, lean, look-at, etc.
    REFLECT()
    class CAnimGraphNode_BoneTransform : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Bone Transform"; }
        FStringView GetNodeTooltip() const override { return "Applies a translation/rotation/scale to a named bone, in the bone's local space or in component space."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(150, 90, 70, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Name of the bone to drive. Must exist in the graph's skeleton. */
        PROPERTY(Editable, Category = "Bone", Picker = "Bone")
        FName BoneName;

        /** Frame the offset is interpreted in: Local Bone (parent-relative, cheap) or Component
         *  (entity-root; applied to the global transform via FK and converted back to local). */
        PROPERTY(Editable, Category = "Bone")
        EBoneTransformSpace Space = EBoneTransformSpace::LocalBone;

        /** How the offset is applied: Add (layered additive) or Replace (lerps toward the target by Alpha). */
        PROPERTY(Editable, Category = "Bone")
        EBoneTransformMode Mode = EBoneTransformMode::Add;

        /** Translation offset (or target, in Replace mode), in the selected space. */
        PROPERTY(Editable, Category = "Transform")
        FVector3 Translation = FVector3(0.0f);

        /** Euler rotation in degrees (X = pitch, Y = yaw, Z = roll). Converted to
         *  a quaternion at compile time. */
        PROPERTY(Editable, Category = "Transform")
        FVector3 Rotation = FVector3(0.0f);

        /** Scale. In Add mode, 1.0 leaves the bone unchanged. */
        PROPERTY(Editable, Category = "Transform")
        FVector3 Scale = FVector3(1.0f);

        CAnimGraphPin* PoseInPin = nullptr;
        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* SpacePin = nullptr;
        CAnimGraphPin* ModePin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };
}
