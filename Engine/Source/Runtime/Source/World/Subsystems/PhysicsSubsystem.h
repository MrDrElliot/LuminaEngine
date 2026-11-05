#pragma once

#include "Containers/Vector.h"
#include "Core/Math/AABB.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Physics/PhysicsScene.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "PhysicsSubsystem.generated.h"

namespace Lumina
{
    // Physics queries and body control against the world's physics scene.
    REFLECT()
    class RUNTIME_API CPhysicsSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        //~ Ray and shape queries.

        // A miss keeps Start and End, while bHit marks whether the remaining hit fields are valid.
        FUNCTION()
        SRayResult Raycast(FVector3 Start, FVector3 End,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        /** Every body the segment crosses, sorted near to far, one entry per body. */
        FUNCTION()
        void RaycastAll(FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity,
            ECollisionProfiles LayerMask, TVector<SRayResult>& Out);

        // Raycast from Start along Direction for Distance meters.
        FUNCTION()
        SRayResult RaycastDirection(FVector3 Start, FVector3 Direction, float Distance,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // True when nothing blocks the segment, stopping at the first blocker. Both entities are skipped, the looker and what it looks at.
        FUNCTION()
        bool HasLineOfSight(FVector3 From, FVector3 To, ECS::FEntity IgnoreEntity,
            ECS::FEntity TargetEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // The segment against one entity's body only, such as testing whether a click landed on it.
        FUNCTION()
        SRayResult RaycastEntity(ECS::FEntity Entity, FVector3 Start, FVector3 End);

        //~ Sweeps move a shape along the segment, and the hit Location is where it first touches something.

        /** Every body a sphere of Radius touches along the segment, near to far. */
        FUNCTION()
        void SphereCast(FVector3 Start, FVector3 End, float Radius,
            ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        SRayResult SphereCastClosest(FVector3 Start, FVector3 End, float Radius,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        SRayResult BoxCast(FVector3 Start, FVector3 End, FVector3 HalfExtents, FQuat Rotation,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void BoxCastAll(FVector3 Start, FVector3 End, FVector3 HalfExtents, FQuat Rotation,
            ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // The capsule runs along Rotation's up axis, the shape of a character moving through the world.
        FUNCTION()
        SRayResult CapsuleCast(FVector3 Start, FVector3 End, float Radius, float HalfHeight, FQuat Rotation,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void CapsuleCastAll(FVector3 Start, FVector3 End, float Radius, float HalfHeight, FQuat Rotation,
            ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // Any shape, for a caller that picks the shape at runtime.
        FUNCTION()
        SRayResult ShapeCast(SQueryShape Shape, FVector3 Start, FVector3 End,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void ShapeCastAll(SQueryShape Shape, FVector3 Start, FVector3 End,
            ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        //~ Overlaps list distinct entities whose bodies intersect the shape. IgnoreEntity excludes the querier.

        FUNCTION()
        void OverlapSphere(FVector3 Center, float Radius, ECS::FEntity IgnoreEntity,
            TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void OverlapBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
            ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void OverlapCapsule(FVector3 Center, float Radius, float HalfHeight, FQuat Rotation,
            ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        void OverlapShape(SQueryShape Shape, FVector3 Center, ECS::FEntity IgnoreEntity,
            TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        /** Entities whose bodies contain the point, which is volume containment rather than a shape sweep. */
        FUNCTION()
        void OverlapPoint(FVector3 Point, ECS::FEntity IgnoreEntity,
            TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // True when no body overlaps the sphere, the check for whether something can be placed there.
        FUNCTION()
        bool IsSpaceFree(FVector3 Center, float Radius, ECS::FEntity IgnoreEntity,
            ECollisionProfiles LayerMask = ECollisionProfiles::All);

        FUNCTION()
        bool IsBoxSpaceFree(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // Whether a character capsule fits at Center, such as before a spawn or a teleport.
        FUNCTION()
        bool IsCapsuleSpaceFree(FVector3 Center, float Radius, float HalfHeight, FQuat Rotation,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        //~ Proximity, measured to the surface of an entity's colliders rather than to its origin.

        // The entity in Radius whose surface is closest to Center, or the null entity when there is none.
        FUNCTION()
        ECS::FEntity FindNearestEntity(FVector3 Center, float Radius, ECS::FEntity IgnoreEntity,
            ECollisionProfiles LayerMask = ECollisionProfiles::All);

        // Point itself when it is inside the body, and Point unchanged when the entity has no body.
        FUNCTION()
        FVector3 GetClosestPoint(ECS::FEntity Entity, FVector3 Point);

        // Zero inside the body, and negative when the entity has no body.
        FUNCTION()
        float GetDistanceToEntity(ECS::FEntity Entity, FVector3 Point);

        // The world bounds of the entity's colliders, an invalid box when it has no body.
        FUNCTION()
        SAABB GetBodyBounds(ECS::FEntity Entity);

        //~ Shape and settings. A direct field write on a physics component takes effect once the entity is rebuilt.

        // Re-applies the entity's body, collider and character settings after their fields were written directly, keeping its motion.
        FUNCTION()
        void RebuildPhysics(ECS::FEntity Entity);

        // Resizes the entity's character capsule, or its capsule collider when it is not a character.
        FUNCTION()
        void SetCapsuleSize(ECS::FEntity Entity, float Radius, float HalfHeight);

        FUNCTION()
        void SetBoxHalfExtent(ECS::FEntity Entity, FVector3 HalfExtent);

        FUNCTION()
        void SetSphereRadius(ECS::FEntity Entity, float Radius);

        // Moves the entity's body or character onto another profile, which changes what it collides with.
        FUNCTION()
        void SetCollisionProfile(ECS::FEntity Entity, SCollisionProfile Profile);

        // The height of the first surface straight down through Point, searching SearchDistance above and below it.
        FUNCTION()
        float GetGroundHeight(FVector3 Point, float SearchDistance, ECS::FEntity IgnoreEntity, float Fallback,
            ECollisionProfiles LayerMask = ECollisionProfiles::All);

        //~ Forces and impulses. An entity without a body is a no-op.

        FUNCTION()
        void AddForce(ECS::FEntity Entity, FVector3 Force);

        FUNCTION()
        void AddImpulse(ECS::FEntity Entity, FVector3 Impulse);

        FUNCTION()
        void AddTorque(ECS::FEntity Entity, FVector3 Torque);

        FUNCTION()
        void AddAngularImpulse(ECS::FEntity Entity, FVector3 AngularImpulse);

        FUNCTION()
        void AddForceAtPosition(ECS::FEntity Entity, FVector3 Force, FVector3 Position);

        FUNCTION()
        void AddImpulseAtPosition(ECS::FEntity Entity, FVector3 Impulse, FVector3 Position);

        //~ Body state. A getter on an entity without a body returns zero.

        FUNCTION()
        void SetLinearVelocity(ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION()
        FVector3 GetLinearVelocity(ECS::FEntity Entity);

        FUNCTION()
        void SetAngularVelocity(ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION()
        FVector3 GetAngularVelocity(ECS::FEntity Entity);

        FUNCTION()
        FVector3 GetVelocityAtPoint(ECS::FEntity Entity, FVector3 Point);

        FUNCTION()
        FVector3 GetBodyPosition(ECS::FEntity Entity);

        FUNCTION()
        FQuat GetBodyRotation(ECS::FEntity Entity);

        FUNCTION()
        FVector3 GetCenterOfMass(ECS::FEntity Entity);

        // Zero for a body that cannot move, since and kinematic bodies have no finite mass.
        FUNCTION()
        float GetMass(ECS::FEntity Entity);

        //~ One body by target, such as a ragdoll limb or the body a raycast hit. A stale or unset target is a no-op.

        // The ragdoll limb simulating Bone, or its nearest ancestor with a body. Unset until the entity's ragdoll simulates.
        FUNCTION()
        SPhysicsBodyTarget GetRagdollBody(ECS::FEntity Entity, FName Bone);

        FUNCTION()
        void AddImpulseToBody(SPhysicsBodyTarget Body, FVector3 Impulse, FVector3 Position);

        FUNCTION()
        void SetBodyLinearVelocity(SPhysicsBodyTarget Body, FVector3 Velocity);

        FUNCTION()
        void SetBodyAngularVelocity(SPhysicsBodyTarget Body, FVector3 Velocity);

        FUNCTION()
        FVector3 GetBodyLinearVelocity(SPhysicsBodyTarget Body);

        FUNCTION()
        void SetGravityFactor(ECS::FEntity Entity, float Factor);

        FUNCTION()
        void ActivateBody(ECS::FEntity Entity);

        FUNCTION()
        void DeactivateBody(ECS::FEntity Entity);

        /** Awake rather than asleep. False when the entity has no body. */
        FUNCTION()
        bool IsAwake(ECS::FEntity Entity);

        /** Bodies the solver is simulating, the asleep ones left out. One call, rather than IsAwake on every body. */
        FUNCTION()
        int32 GetAwakeBodyCount();

        /** Drags bodies resting on this collider, the conveyor belt. Zero clears it. */
        FUNCTION()
        void SetSurfaceVelocity(ECS::FEntity Entity, FVector3 Linear, FVector3 Angular);

        //~ Constraints. A body left as the null entity anchors that side to the world.

        FUNCTION()
        uint32 CreateConstraint(Physics::SConstraintDesc Desc);

        /** Welds the two bodies rigidly at their current relative pose. */
        FUNCTION()
        uint32 CreateFixedConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            float BreakForce = 0.0f);

        /** Ball and socket pinned at Pivot, which removes the three translation degrees of freedom. */
        FUNCTION()
        uint32 CreatePointConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, float BreakForce = 0.0f);

        /** Holds PointA and PointB between the two distances. A negative limit means the current distance. */
        FUNCTION()
        uint32 CreateDistanceConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 PointA, FVector3 PointB, float MinDistance = -1.0f, float MaxDistance = -1.0f,
            float Frequency = 0.0f, float Damping = 0.0f, float BreakForce = 0.0f);

        /** Single axis hinge at Pivot. Angles are radians and only apply when bLimited is set. */
        FUNCTION()
        uint32 CreateHingeConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 Axis, float MinAngle = 0.0f, float MaxAngle = 0.0f, bool bLimited = false,
            float MaxFrictionTorque = 0.0f, float MotorTorqueLimit = 0.0f, float MotorFrequency = 0.0f,
            float MotorDamping = 0.0f, float BreakForce = 0.0f);

        /** Prismatic slider along Axis. Distances are meters and only apply when bLimited is set. */
        FUNCTION()
        uint32 CreateSliderConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 Axis, float MinDistance = 0.0f, float MaxDistance = 0.0f,
            bool bLimited = false, float MaxFrictionForce = 0.0f, float MotorForceLimit = 0.0f,
            float MotorFrequency = 0.0f, float MotorDamping = 0.0f, float BreakForce = 0.0f);

        /** Swing limited ball and socket, keeping B's twist axis within HalfConeAngle radians of A's. */
        FUNCTION()
        uint32 CreateConeConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 TwistAxis, float HalfConeAngle, float BreakForce = 0.0f);

        FUNCTION()
        void DestroyConstraint(uint32 ConstraintID);

        FUNCTION()
        void SetConstraintEnabled(uint32 ConstraintID, bool bEnabled);

        FUNCTION()
        void SetConstraintMotor(uint32 ConstraintID, Physics::EConstraintMotorMode Mode,
            float Target);

        FUNCTION()
        bool IsConstraintBroken(uint32 ConstraintID);

        /** A hinge reports radians and a slider meters. Every other type reports zero. */
        FUNCTION()
        float GetConstraintValue(uint32 ConstraintID);
    };
}
