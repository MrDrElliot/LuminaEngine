#pragma once

#include "Core/LuminaMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Physics/PhysicsTypes.h"
#include "PhysicsAsset.generated.h"

namespace Lumina
{
    class CSkeleton;
    class CPhysicsMaterial;

    /** Collision primitive used for a ragdoll body. Capsule is the default limb shape. */
    REFLECT()
    enum class RUNTIME_API ERagdollBodyShape : uint8
    {
        Capsule,
        Box,
        Sphere,
    };

    /** One simulated rigid body of a ragdoll, bound to a skeleton bone. The body frame is the bone
     *  frame offset by Translation/RotationOffset (e.g. a capsule slid halfway down toward the child). */
    REFLECT()
    struct RUNTIME_API SPhysicsBodySetup
    {
        GENERATED_BODY()

        /** Skeleton bone this body is attached to and reads/writes its transform from. */
        PROPERTY(Editable, Category = "Body")
        FName BoneName;

        /** Collision primitive for this body. */
        PROPERTY(Editable, Category = "Body")
        ERagdollBodyShape Shape = ERagdollBodyShape::Capsule;

        /** Capsule/Sphere radius, and capsule cap radius (meters). */
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Body|Shape")
        float Radius = 0.06f;

        /** Capsule cylindrical half-height (meters); total capsule height is 2*(HalfHeight + Radius). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Body|Shape")
        float HalfHeight = 0.12f;

        /** Box half-extent (meters) when Shape is Box. */
        PROPERTY(Editable, Category = "Body|Shape")
        FVector3 HalfExtent = FVector3(0.06f);

        /** Body-frame translation offset from the bone origin (bone local space, meters). */
        PROPERTY(Editable, Category = "Body|Frame")
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Body-frame euler rotation offset from the bone (degrees). */
        PROPERTY(Editable, Category = "Body|Frame")
        FVector3 RotationOffset = FVector3(0.0f);

        /** Body mass (kg) when bOverrideMass is set; otherwise Jolt computes it from shape + density. */
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Body")
        float Mass = 1.0f;

        /** Use Mass instead of the density-computed mass. */
        PROPERTY(Editable, Category = "Body")
        bool bOverrideMass = false;

        /** Surface material; null falls back to the asset's defaults. */
        PROPERTY(Editable, Category = "Body")
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;
    };

    /** A swing-twist joint connecting a child body to its parent body, limiting how far the limb can
     *  rotate. Authored by bone names; the pivot is the child bone's origin. */
    REFLECT()
    struct RUNTIME_API SPhysicsConstraintSetup
    {
        GENERATED_BODY()

        /** Parent body's bone (closer to the root). */
        PROPERTY(Editable, Category = "Constraint")
        FName ParentBone;

        /** Child body's bone (the limb being constrained). */
        PROPERTY(Editable, Category = "Constraint")
        FName ChildBone;

        /** Half-angle the twist axis may rotate (degrees). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float TwistLimitDegrees = 30.0f;

        /** Cone half-angle about the first swing axis (degrees). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float Swing1LimitDegrees = 45.0f;

        /** Cone half-angle about the second swing axis (degrees). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 180.0f, Category = "Constraint|Limits")
        float Swing2LimitDegrees = 45.0f;

        /** Motor frequency (Hz) used when the ragdoll is driven to a pose by motors. 0 = no motor (Phase 2+). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Constraint|Motor")
        float MotorFrequency = 0.0f;

        /** Motor damping ratio used with MotorFrequency. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Constraint|Motor")
        float MotorDamping = 1.0f;
    };

    /** Authored ragdoll definition for a skeleton: which bones get simulated bodies and how the joints
     *  between them are limited. Reusable across every skeletal mesh that shares the Skeleton.
     *  An empty asset can be auto-populated from the skeleton (capsule per bone, swing-twist per joint). */
    REFLECT()
    class RUNTIME_API CPhysicsAsset : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        /** Skeleton these bodies/constraints are authored against. */
        PROPERTY(Editable, Category = "Physics Asset")
        TObjectPtr<CSkeleton> Skeleton;

        /** Collision layer/mask applied to every ragdoll body. */
        PROPERTY(Editable, Category = "Physics Asset")
        FCollisionProfile CollisionProfile;

        /** Simulated bodies, one per driven bone. */
        PROPERTY(Editable, Category = "Physics Asset")
        TVector<SPhysicsBodySetup> Bodies;

        /** Joints connecting child bodies to their parents. */
        PROPERTY(Editable, Category = "Physics Asset")
        TVector<SPhysicsConstraintSetup> Constraints;

        /** Index into Bodies for a bone name, or INDEX_NONE. */
        int32 FindBodyIndex(const FName& BoneName) const;
    };
}
