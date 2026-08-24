#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_OrientationWarping.generated.h"

namespace Lumina
{
    // Where an orientation warp reads the angle it turns the lower body by.
    REFLECT()
    enum class EOrientationAngleSource : uint8
    {
        /** Taken from the entity's own velocity, so a walking character needs nothing wired. */
        Velocity,

        /** Taken from the Angle pin, for warping onto an aim or a path heading instead. */
        Pin,
    };

    // One bone the counter-rotation is spread across. Weights are relative, not required to sum to one.
    REFLECT()
    struct SAnimOrientationSpineBone
    {
        GENERATED_BODY()

        /** Spine bone that takes part of the counter-rotation. Must sit above the pelvis. */
        PROPERTY(Editable, Category = "Spine", Picker = "Bone")
        FName Bone;

        /** Share of the counter-rotation this bone carries, relative to the others. */
        PROPERTY(Editable, Category = "Spine", ClampMin = 0.0f)
        float Weight = 1.0f;
    };

    // Turns the lower body onto the movement direction and counter-rotates the spine to hold the aim.
    REFLECT()
    class CAnimGraphNode_OrientationWarping : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Orientation Warping"; }
        FStringView GetNodeTooltip() const override { return "Rotates the pelvis onto the direction the character is actually moving, then counter-rotates the spine so the torso keeps facing forward. Lets one forward locomotion set cover strafing and diagonal movement instead of authoring a clip per direction."; }
        FFixedString GetNodeCategory() const override { return "Animation|Warping"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(120, 90, 160, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Bone the whole lower body turns on. Every spine bone below must descend from it. */
        PROPERTY(Editable, Category = "Orientation Warping", Picker = "Bone")
        FName PelvisBone;

        /** Bones the counter-rotation is spread across. More bones curve the twist instead of kinking it. */
        PROPERTY(Editable, Category = "Orientation Warping")
        TVector<SAnimOrientationSpineBone> SpineBones;

        /** Where the angle comes from. */
        PROPERTY(Editable, Category = "Orientation Warping")
        EOrientationAngleSource AngleSource = EOrientationAngleSource::Velocity;

        /** How much of the pelvis turn the spine takes back. 1 holds the torso exactly where it was. */
        PROPERTY(Editable, Category = "Orientation Warping", ClampMin = 0.0f, ClampMax = 1.0f)
        float CounterRotation = 1.0f;

        /** Axis the body turns about, in component space. */
        PROPERTY(Editable, Category = "Orientation Warping")
        FVector3 RotationAxis = FVector3(0.0f, 1.0f, 0.0f);

        /** Direction the animation itself faces, in component space. The angle is measured from here. */
        PROPERTY(Editable, Category = "Orientation Warping")
        FVector3 ForwardAxis = FVector3(0.0f, 0.0f, 1.0f);

        /** Largest angle the body may turn. Beyond it the legs outrun the animation and cross over. */
        PROPERTY(Editable, Category = "Orientation Warping", ClampMin = 0.0f, ClampMax = 180.0f, Units = "Degrees")
        float MaxAngle = 90.0f;

        /** Movement slower than this reads as standing still, so noise cannot spin the hips. */
        PROPERTY(Editable, Category = "Velocity", ClampMin = 0.0f, Units = "m/s")
        float MinSpeed = 0.1f;

        /** Seconds for the angle to cover half the distance to its target. 0 snaps to every change. */
        PROPERTY(Editable, Category = "Orientation Warping", ClampMin = 0.0f, Units = "Seconds")
        float SmoothingHalfLife = 0.15f;

        CAnimGraphPin* PoseInPin = nullptr;

        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* AnglePin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };

    // The angle a warp node computes, exposed on its own so a blend space can steer by it too.
    REFLECT()
    class CAnimGraphNode_MovementAngle : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Movement Angle"; }
        FStringView GetNodeTooltip() const override { return "Signed angle from the character's forward axis to the direction it is actually moving, measured about the turn axis. Feed it to a strafe blend space, or to an Orientation Warping node set to take its angle from a pin."; }
        FFixedString GetNodeCategory() const override { return "Animation|Warping"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(120, 90, 160, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Axis the angle is measured about, in component space. */
        PROPERTY(Editable, Category = "Movement Angle")
        FVector3 RotationAxis = FVector3(0.0f, 1.0f, 0.0f);

        /** Direction that reads as zero, in component space. */
        PROPERTY(Editable, Category = "Movement Angle")
        FVector3 ForwardAxis = FVector3(0.0f, 0.0f, 1.0f);

        /** Movement slower than this reads as zero rather than as a direction. */
        PROPERTY(Editable, Category = "Movement Angle", ClampMin = 0.0f, Units = "m/s")
        float MinSpeed = 0.1f;

        /** Degrees rather than radians, which is what a blend space axis is usually authored in. */
        PROPERTY(Editable, Category = "Movement Angle")
        bool bOutputDegrees = true;

        /** Seconds for the angle to cover half the distance to its target. 0 snaps to every change. */
        PROPERTY(Editable, Category = "Movement Angle", ClampMin = 0.0f, Units = "Seconds")
        float SmoothingHalfLife = 0.15f;

        CAnimGraphPin* AngleOutPin = nullptr;
    };
}
