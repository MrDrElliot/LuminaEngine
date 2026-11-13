#include "PhysicsAsset.h"

namespace Lumina
{
    FVector3 SPhysicsConstraintSetup::GetLimitsRadians() const
    {
        return Math::Radians(FVector3(TwistLimitDegrees, Swing1LimitDegrees, Swing2LimitDegrees));
    }

    float SPhysicsConstraintSetup::GetSoftLimitStart() const
    {
        return bSoftLimits ? SoftLimitStart : 1.0f;
    }

    float ClampToSoftRange(FQuat& JointRotation, const FVector3& Limits, float SoftStart)
    {
        FQuat Rotation = Math::Normalize(JointRotation);
        if (Rotation.w < 0.0f)
        {
            Rotation = FQuat(-Rotation.w, -Rotation.x, -Rotation.y, -Rotation.z);
        }

        // Swing then twist about the frame's Z axis, the split Box3D's spherical joint limits by.
        const float TwistLength = Math::Sqrt(Rotation.z * Rotation.z + Rotation.w * Rotation.w);
        const FQuat Twist = TwistLength > 1e-6f ? FQuat(Rotation.w / TwistLength, 0.0f, 0.0f, Rotation.z / TwistLength) : FQuat::Identity();
        const FQuat Swing = Rotation * Math::Inverse(Twist);
        const float TwistAngle = 2.0f * Math::Atan2(Twist.z, Twist.w);

        const float SinHalfSwing = Math::Sqrt(Swing.x * Swing.x + Swing.y * Swing.y);
        const float SwingAngle = 2.0f * Math::Atan2(SinHalfSwing, Swing.w);
        const FVector3 SwingVector = SinHalfSwing > 1e-6f ? FVector3(Swing.x, Swing.y, 0.0f) * (SwingAngle / SinHalfSwing) : FVector3(0.0f);

        // How far out the joint reaches as a share of its limits, the swing measured against the ellipse in its direction.
        const float SwingReach = Math::Length(SwingVector / FVector3(Math::Max(Limits.y, 1e-4f), Math::Max(Limits.z, 1e-4f), 1.0f));
        const float TwistReach = Math::Abs(TwistAngle) / Math::Max(Limits.x, 1e-4f);
        if (SwingReach <= SoftStart && TwistReach <= SoftStart)
        {
            return 0.0f;
        }

        const FVector3 ClampedSwing = SwingReach > SoftStart ? SwingVector * (SoftStart / SwingReach) : SwingVector;
        const float ClampedSwingAngle = Math::Length(ClampedSwing);
        const FQuat SwingRotation = ClampedSwingAngle > 1e-6f ? Math::AngleAxis(ClampedSwingAngle, ClampedSwing / ClampedSwingAngle) : FQuat::Identity();
        const float ClampedTwist = Math::Clamp(TwistAngle, -Limits.x * SoftStart, Limits.x * SoftStart);
        JointRotation = Math::Normalize(SwingRotation * Math::AngleAxis(ClampedTwist, FVector3(0.0f, 0.0f, 1.0f)));

        return SoftStart < 1.0f ? Math::Min((Math::Max(SwingReach, TwistReach) - SoftStart) / (1.0f - SoftStart), 1.0f) : 1.0f;
    }

    FQuat MakeRagdollJointFrame(const FVector3& TowardChildren, const FVector3& AwayFromParent, const FVector3& BoneUp)
    {
        const FVector3 Along = Math::Length(TowardChildren) > 1e-4f ? TowardChildren : AwayFromParent;
        const FVector3 TwistAxis = Math::Length(Along) > 1e-4f ? Math::Normalize(Along) : Math::Normalize(BoneUp);

        // The same perpendicular a debug arc around the twist axis starts from, so drawn twist angles line up with the frame.
        const FVector3 Reference = Math::Abs(TwistAxis.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
        const FVector3 Perp = Math::Normalize(Math::Cross(Reference, TwistAxis));
        const FVector3 Bitangent = Math::Cross(TwistAxis, Perp);
        return Math::ToQuat(FMatrix4(FVector4(Perp, 0.0f), FVector4(Bitangent, 0.0f), FVector4(TwistAxis, 0.0f), FVector4(0.0f, 0.0f, 0.0f, 1.0f)));
    }

    SPhysicsConstraintSetup FindConstraintSetup(TSpan<const SPhysicsConstraintSetup> Constraints, const FName& ChildBone)
    {
        for (const SPhysicsConstraintSetup& Constraint : Constraints)
        {
            if (Constraint.ChildBone == ChildBone)
            {
                return Constraint;
            }
        }
        return SPhysicsConstraintSetup();
    }

    int32 CPhysicsAsset::FindBodyIndex(const FName& BoneName) const
    {
        for (int32 i = 0; i < Bodies.Num(); ++i)
        {
            if (Bodies[i].BoneName == BoneName)
            {
                return i;
            }
        }
        return Constants::kIndexNone;
    }
}
