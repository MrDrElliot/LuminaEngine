#pragma once

#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Physics/PhysicsScene.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "World/ECS/Entity.h"
#include "PhysicsLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    /** Queries and body control against a world's physics scene, which answer about bodies rather than the world. */
    REFLECT()
    class RUNTIME_API CPhysicsLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        //~ Ray and shape queries.

        /** Closest hit along the segment. A miss comes back zeroed, so read bHit before the rest. */
        FUNCTION()
        static SRayResult Raycast(CWorld* World, FVector3 Start, FVector3 End,
            ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask = ECollisionProfiles::All);

        /** Every body the segment crosses, sorted near to far, one entry per body. */
        FUNCTION()
        static void RaycastAll(CWorld* World, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity,
            ECollisionProfiles LayerMask, TVector<SRayResult>& Out);

        /** A thick raycast, sweeping a sphere of Radius along the segment. */
        FUNCTION()
        static void SphereCast(CWorld* World, FVector3 Start, FVector3 End, float Radius,
            ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out);

        /** Distinct entities whose bodies overlap the sphere. IgnoreEntity excludes the querier. */
        FUNCTION()
        static void OverlapSphere(CWorld* World, FVector3 Center, float Radius, ECS::FEntity IgnoreEntity,
            TVector<ECS::FEntity>& Out);

        /** The same against an oriented box. */
        FUNCTION()
        static void OverlapBox(CWorld* World, FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
            ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out);

        /** Entities whose bodies contain the point, which is volume containment rather than a shape sweep. */
        FUNCTION()
        static void OverlapPoint(CWorld* World, FVector3 Point, ECS::FEntity IgnoreEntity,
            TVector<ECS::FEntity>& Out);

        //~ Forces and impulses. An entity without a body is a no-op.

        FUNCTION()
        static void AddForce(CWorld* World, ECS::FEntity Entity, FVector3 Force);

        FUNCTION()
        static void AddImpulse(CWorld* World, ECS::FEntity Entity, FVector3 Impulse);

        FUNCTION()
        static void AddTorque(CWorld* World, ECS::FEntity Entity, FVector3 Torque);

        FUNCTION()
        static void AddAngularImpulse(CWorld* World, ECS::FEntity Entity, FVector3 AngularImpulse);

        FUNCTION()
        static void AddForceAtPosition(CWorld* World, ECS::FEntity Entity, FVector3 Force, FVector3 Position);

        FUNCTION()
        static void AddImpulseAtPosition(CWorld* World, ECS::FEntity Entity, FVector3 Impulse, FVector3 Position);

        //~ Body state. A getter on an entity without a body returns zero.

        FUNCTION()
        static void SetLinearVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION()
        static FVector3 GetLinearVelocity(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static void SetAngularVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION()
        static FVector3 GetAngularVelocity(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static FVector3 GetVelocityAtPoint(CWorld* World, ECS::FEntity Entity, FVector3 Point);

        FUNCTION()
        static FVector3 GetBodyPosition(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static FQuat GetBodyRotation(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static FVector3 GetCenterOfMass(CWorld* World, ECS::FEntity Entity);

        // Zero for a body that cannot move, since static and kinematic bodies have no finite mass.
        FUNCTION()
        static float GetMass(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static void SetGravityFactor(CWorld* World, ECS::FEntity Entity, float Factor);


        FUNCTION()
        static void ActivateBody(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static void DeactivateBody(CWorld* World, ECS::FEntity Entity);

        /** Awake rather than asleep. False when the entity has no body. */
        FUNCTION()
        static bool IsAwake(CWorld* World, ECS::FEntity Entity);

        /** Drags bodies resting on this collider, the conveyor belt. Zero clears it. */
        FUNCTION()
        static void SetSurfaceVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Linear, FVector3 Angular);

        //~ Constraints. A body left as the null entity anchors that side to the world.

        FUNCTION()
        static uint32 CreateConstraint(CWorld* World, Physics::FConstraintDesc Desc);

        /** Welds the two bodies rigidly at their current relative pose. */
        FUNCTION()
        static uint32 CreateFixedConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            float BreakForce = 0.0f);

        /** Ball and socket pinned at Pivot, which removes the three translation degrees of freedom. */
        FUNCTION()
        static uint32 CreatePointConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, float BreakForce = 0.0f);

        /** Holds PointA and PointB between the two distances. A negative limit means the current distance. */
        FUNCTION()
        static uint32 CreateDistanceConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 PointA, FVector3 PointB, float MinDistance = -1.0f, float MaxDistance = -1.0f,
            float Frequency = 0.0f, float Damping = 0.0f, float BreakForce = 0.0f);

        /** Single axis hinge at Pivot. Angles are radians and only apply when bLimited is set. */
        FUNCTION()
        static uint32 CreateHingeConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 Axis, float MinAngle = 0.0f, float MaxAngle = 0.0f, bool bLimited = false,
            float MaxFrictionTorque = 0.0f, float MotorTorqueLimit = 0.0f, float MotorFrequency = 0.0f,
            float MotorDamping = 0.0f, float BreakForce = 0.0f);

        /** Prismatic slider along Axis. Distances are meters and only apply when bLimited is set. */
        FUNCTION()
        static uint32 CreateSliderConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 Axis, float MinDistance = 0.0f, float MaxDistance = 0.0f,
            bool bLimited = false, float MaxFrictionForce = 0.0f, float MotorForceLimit = 0.0f,
            float MotorFrequency = 0.0f, float MotorDamping = 0.0f, float BreakForce = 0.0f);

        /** Swing limited ball and socket, keeping B's twist axis within HalfConeAngle radians of A's. */
        FUNCTION()
        static uint32 CreateConeConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
            FVector3 Pivot, FVector3 TwistAxis, float HalfConeAngle, float BreakForce = 0.0f);

        FUNCTION()
        static void DestroyConstraint(CWorld* World, uint32 ConstraintID);

        FUNCTION()
        static void SetConstraintEnabled(CWorld* World, uint32 ConstraintID, bool bEnabled);

        FUNCTION()
        static void SetConstraintMotor(CWorld* World, uint32 ConstraintID, Physics::EConstraintMotorMode Mode,
            float Target);

        FUNCTION()
        static bool IsConstraintBroken(CWorld* World, uint32 ConstraintID);

        /** A hinge reports radians and a slider meters. Every other type reports zero. */
        FUNCTION()
        static float GetConstraintValue(CWorld* World, uint32 ConstraintID);
    };
}
