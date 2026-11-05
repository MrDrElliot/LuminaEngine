#pragma once

#include "Containers/Vector.h"
#include "Core/Math/AABB.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "SystemContext.h"
#include "SystemPhysicsLibrary.generated.h"

namespace Lumina
{
    // The IPhysicsQueries a C# system reaches through its SSystemContext, so a parallel one never touches the world or a physics subsystem.
    REFLECT()
    class RUNTIME_API CSystemPhysicsLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        //~ Queries. With no physics scene a cast misses and an overlap finds nothing.

        FUNCTION(ScriptInternal)
        static SRayResult Raycast(const SSystemContext& Context, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask);

        FUNCTION(ScriptInternal)
        static void RaycastAll(const SSystemContext& Context, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<SRayResult>& Out);

        FUNCTION(ScriptInternal)
        static SRayResult RaycastEntity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Start, FVector3 End);

        FUNCTION(ScriptInternal)
        static SRayResult ShapeCast(const SSystemContext& Context, SQueryShape Shape, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask);

        FUNCTION(ScriptInternal)
        static void ShapeCastAll(const SSystemContext& Context, SQueryShape Shape, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<SRayResult>& Out);

        FUNCTION(ScriptInternal)
        static void Overlap(const SSystemContext& Context, SQueryShape Shape, FVector3 Center, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<ECS::FEntity>& Out);

        //~ Body reads.

        FUNCTION(ScriptInternal)
        static bool IsAwake(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static int32 GetAwakeBodyCount(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static FVector3 GetBodyPosition(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static FQuat GetBodyRotation(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static FVector3 GetLinearVelocity(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static FVector3 GetAngularVelocity(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static FVector3 GetVelocityAtPoint(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Point);

        FUNCTION(ScriptInternal)
        static FVector3 GetCenterOfMass(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static float GetMass(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static SAABB GetBodyBounds(const SSystemContext& Context, ECS::FEntity Entity);

        //~ Body commands, queued for the next physics step.

        FUNCTION(ScriptInternal)
        static void AddForce(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Force);

        FUNCTION(ScriptInternal)
        static void AddImpulse(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Impulse);

        FUNCTION(ScriptInternal)
        static void AddTorque(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Torque);

        FUNCTION(ScriptInternal)
        static void AddAngularImpulse(const SSystemContext& Context, ECS::FEntity Entity, FVector3 AngularImpulse);

        FUNCTION(ScriptInternal)
        static void AddForceAtPosition(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Force, FVector3 Position);

        FUNCTION(ScriptInternal)
        static void AddImpulseAtPosition(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Impulse, FVector3 Position);

        FUNCTION(ScriptInternal)
        static void SetLinearVelocity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION(ScriptInternal)
        static void SetAngularVelocity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Velocity);

        FUNCTION(ScriptInternal)
        static void SetGravityFactor(const SSystemContext& Context, ECS::FEntity Entity, float Factor);
    };
}
