#pragma once

#include "Core/LuminaMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Physics/PhysicsTypes.h"
#include "PhysicsAsset.generated.h"

namespace Lumina
{
    class CSkeleton;
    class CPhysicsMaterial;

    // Collision primitive used for a ragdoll body. Capsule is the default limb shape.
    REFLECT()
    enum class ERagdollBodyShape : uint8
    {
        Capsule,
        Box,
        Sphere,
    };

    // One ragdoll body on a skeleton bone, its frame the bone's offset by TranslationOffset and RotationOffset.
    REFLECT()
    struct RUNTIME_API SPhysicsBodySetup
    {
        GENERATED_BODY()

        // Skeleton bone this body is attached to and reads/writes its transform from.
        PROPERTY(Editable, Category = "Body")
        FName BoneName;

        // Collision primitive for this body.
        PROPERTY(Editable, Category = "Body")
        ERagdollBodyShape Shape = ERagdollBodyShape::Capsule;

        // Capsule/Sphere radius, and capsule cap radius (meters).
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Body|Shape")
        float Radius = 0.06f;

        // Capsule cylindrical half-height (meters); total capsule height is 2*(HalfHeight + Radius).
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Body|Shape")
        float HalfHeight = 0.12f;

        // Box half-extent (meters) when Shape is Box.
        PROPERTY(Editable, Category = "Body|Shape")
        FVector3 HalfExtent = FVector3(0.06f);

        // Body-frame translation offset from the bone origin (bone local space, meters).
        PROPERTY(Editable, Category = "Body|Frame")
        FVector3 TranslationOffset = FVector3(0.0f);

        // Body-frame euler rotation offset from the bone (degrees).
        PROPERTY(Editable, Category = "Body|Frame")
        FVector3 RotationOffset = FVector3(0.0f);

        // Body mass (kg) when bOverrideMass is set; otherwise it is computed from shape and density.
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Body")
        float Mass = 1.0f;

        // Use Mass instead of the density-computed mass.
        PROPERTY(Editable, Category = "Body")
        bool bOverrideMass = false;

        // Surface material; null falls back to the asset's defaults.
        PROPERTY(Editable, Category = "Body")
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;
    };

    // A swing-twist joint limiting a child body against its parent, pivoting at the child bone's origin.
    REFLECT()
    struct RUNTIME_API SPhysicsConstraintSetup
    {
        GENERATED_BODY()

        // Parent body's bone (closer to the root).
        PROPERTY(Editable, Category = "Constraint")
        FName ParentBone;

        // Child body's bone (the limb being constrained).
        PROPERTY(Editable, Category = "Constraint")
        FName ChildBone;

        // Half-angle the twist axis may rotate (degrees).
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float TwistLimitDegrees = 30.0f;

        // Swing half-angle about the joint frame's first axis (degrees), with Swing2 making an elliptical cone.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float Swing1LimitDegrees = 45.0f;

        // Swing half-angle about the joint frame's second axis (degrees), with Swing1 making an elliptical cone.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float Swing2LimitDegrees = 45.0f;

        // Ease into the limits like a ligament, a spring that grows past the soft start, instead of meeting a hard stop.
        PROPERTY(Editable, Category = "Constraint|Soft Limits")
        bool bSoftLimits = true;

        // Share of each limit the joint moves freely before the soft limit spring starts pulling it back.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, EditCondition = "bSoftLimits", Category = "Constraint|Soft Limits")
        float SoftLimitStart = 0.7f;

        // How quickly the limb springs back once it reaches the limit, the spring stiffening from nothing at the soft start.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 30.0f, Units = "Hz", EditCondition = "bSoftLimits", Category = "Constraint|Soft Limits")
        float LimitStiffness = 3.0f;

        // Soft limit damping ratio, one settling back without a bounce.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 4.0f, EditCondition = "bSoftLimits", Category = "Constraint|Soft Limits")
        float LimitDampingRatio = 1.0f;

        // Torque the soft limit can pull back with on the strongest joint, lighter limbs getting a share by the mass they carry.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 5000.0f, Units = "N m", EditCondition = "bSoftLimits", Category = "Constraint|Soft Limits")
        float MaxLimitTorque = 300.0f;

        // Muscle stiffness for this joint while powered, zero keeping the ragdoll component's.
        PROPERTY(Editable, ClampMin = 0.0f, Units = "Hz", Category = "Constraint|Motor")
        float MotorFrequency = 0.0f;

        // Muscle damping ratio for this joint while powered, zero keeping the ragdoll component's.
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Constraint|Motor")
        float MotorDamping = 0.0f;

        // Scales this joint's share of the ragdoll's muscle torque, such as a weak neck or strong hips.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 10.0f, Category = "Constraint|Motor")
        float MotorStrength = 1.0f;

        // Twist, Swing1 and Swing2 half-angles in radians.
        FVector3 GetLimitsRadians() const;

        // Share of the limits the soft zone starts at, the limits themselves when soft limits are off.
        float GetSoftLimitStart() const;
    };

    // Clamps a joint frame rotation into the soft range and returns how far through the soft zone it reached, zero inside and one at the limits.
    RUNTIME_API float ClampToSoftRange(FQuat& JointRotation, const FVector3& LimitsRadians, float SoftStart);

    // A ragdoll joint's frame, Z along the limb toward its children (else away from its parent, else BoneUp) and X the swing 1 axis.
    RUNTIME_API FQuat MakeRagdollJointFrame(const FVector3& TowardChildren, const FVector3& AwayFromParent, const FVector3& BoneUp);

    // The constraint on ChildBone, or the defaults for a bone the asset has none for.
    RUNTIME_API SPhysicsConstraintSetup FindConstraintSetup(TSpan<const SPhysicsConstraintSetup> Constraints, const FName& ChildBone);

    // Ragdoll bodies and joint limits for a skeleton, shared by every skeletal mesh on that Skeleton.
    REFLECT()
    class RUNTIME_API CPhysicsAsset : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        // Skeleton these bodies/constraints are authored against.
        PROPERTY(Editable, Category = "Physics Asset")
        TStrongObjectPtr<CSkeleton> Skeleton;

        // Collision layer/mask applied to every ragdoll body.
        PROPERTY(Editable, Category = "Physics Asset")
        SCollisionProfile CollisionProfile;

        // Simulated bodies, one per driven bone.
        PROPERTY(Editable, Category = "Physics Asset")
        TVector<SPhysicsBodySetup> Bodies;

        // Joints connecting child bodies to their parents.
        PROPERTY(Editable, Category = "Physics Asset")
        TVector<SPhysicsConstraintSetup> Constraints;

        // Index into Bodies for a bone name, or Constants::kIndexNone.
        int32 FindBodyIndex(const FName& BoneName) const;
    };
}
