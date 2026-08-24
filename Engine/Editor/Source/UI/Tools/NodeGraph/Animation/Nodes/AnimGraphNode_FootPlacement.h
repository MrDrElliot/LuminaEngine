#pragma once

#include "Physics/Ray/RayCast.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_FootPlacement.generated.h"

namespace Lumina
{
    // One leg traced against the ground. The chain must run thigh to calf to foot without a gap.
    REFLECT()
    struct SAnimFootPlacementLeg
    {
        GENERATED_BODY()

        /** Upper leg. Its child must be the calf. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName ThighBone;

        /** Lower leg, parented to the thigh. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName CalfBone;

        /** Foot, parented to the calf. The trace starts under this bone and the leg solves onto it. */
        PROPERTY(Editable, Category = "Leg", Picker = "Bone")
        FName FootBone;

        /** Height the foot bone sits above the sole in the bind pose, kept out of the ground. */
        PROPERTY(Editable, Category = "Leg", ClampMin = 0.0f, Units = "Centimeters")
        float SoleHeight = 0.0f;
    };

    // Traces under every foot, drops the pelvis onto the lowest, and solves each leg onto the ground.
    REFLECT()
    class CAnimGraphNode_FootPlacement : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Foot Placement"; }
        FStringView GetNodeTooltip() const override { return "Whole-body ground adaptation: traces under each foot, lowers the pelvis to whichever foot had to reach furthest, then solves every leg onto its ground point and rolls the foot onto the surface normal. Replaces a Translate Bone plus one Foot IK per leg, and does its own tracing, so nothing has to be wired in from gameplay."; }
        FFixedString GetNodeCategory() const override { return "Animation|IK"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(160, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Legs traced for. Two for a biped, but nothing here assumes that. */
        PROPERTY(Editable, Category = "Foot Placement")
        TVector<SAnimFootPlacementLeg> Legs;

        /** Bone lowered to keep the legs in reach. Every leg must descend from it. */
        PROPERTY(Editable, Category = "Foot Placement", Picker = "Bone")
        FName PelvisBone;

        /** World up, which the traces run along. Matches the physics gravity direction by default. */
        PROPERTY(Editable, Category = "Foot Placement")
        FVector3 UpAxis = FVector3(0.0f, 1.0f, 0.0f);

        /** Axis of the foot bones that points away from the ground, in a foot's own space. */
        PROPERTY(Editable, Category = "Foot Placement")
        FVector3 FootUpAxis = FVector3(0.0f, 1.0f, 0.0f);

        /** How much of the ground normal the feet roll onto. 0 keeps them level with the animation. */
        PROPERTY(Editable, Category = "Foot Placement", ClampMin = 0.0f, ClampMax = 1.0f)
        float GroundAlignment = 1.0f;

        /** How far above the foot each trace starts, covering ground that rises ahead of the step. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float TraceUpDistance = 50.0f;

        /** How far below the foot a trace reaches before the foot is treated as airborne. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float TraceDownDistance = 100.0f;

        /** Largest displacement any foot may take, so a bad trace cannot tear the leg apart. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float MaxOffset = 60.0f;

        /** Ground the traces collide with. */
        PROPERTY(Editable, Category = "Trace")
        ECollisionProfiles TraceLayerMask = AllCollisionProfiles;

        /** Seconds for an offset to cover half the distance to its traced target. 0 snaps. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Seconds")
        float SmoothingHalfLife = 0.08f;

        CAnimGraphPin* PoseInPin = nullptr;

        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* PoseOutPin = nullptr;
    };
}
