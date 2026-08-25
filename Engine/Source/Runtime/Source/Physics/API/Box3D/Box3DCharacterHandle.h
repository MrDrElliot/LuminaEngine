#pragma once

#include <box3d/box3d.h>
#include "Core/Math/Math.h"
#include "Physics/PhysicsTypes.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Box3D's mover is a stateless collide-and-solve toolkit, so the persistent character state lives here.
    struct FPhysicsCharacterHandle
    {
        b3WorldId   WorldId{};

        // Kinematic proxy so other bodies, queries and contact events see the character as a real body.
        b3BodyId    ProxyBody{};
        b3ShapeId   ProxyShape{};
        uint32      ProxyBodyHandle = 0;

        b3QueryFilter Filter{};

        // Queries decide the pair in a callback, so the authored profile has to travel with the mover.
        FCollisionProfile Profile{};

        FVector3    Position = FVector3(0.0f);
        FQuat       Rotation = FQuat::Identity();
        FVector3    Velocity = FVector3(0.0f);

        FVector3    GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
        FVector3    GroundVelocity = FVector3(0.0f);
        uint32      GroundEntity = 0xFFFFFFFFu;
        bool        bGrounded = false;

        float       StickToFloorDistance = 0.5f;

        // Where the spawner put the entity, kept so the character can be re-seated once the world exists.
        FVector3    SpawnPosition = FVector3(0.0f);

        // A dynamic mesh collider defers until its CPU data is ready, so the ground can appear frames late.
        bool        bAwaitingGround = true;
        uint32      AwaitingGroundSteps = 0;

        float       Radius = 0.35f;
        float       HalfHeight = 0.55f;
        float       Padding = 0.02f;
        FVector3    TranslationOffset = FVector3(0.0f);
        float       StepHeight = 0.4f;
        float       CosMaxSlope = 0.7071f;
        float       MaxStrength = 100.0f;
        float       Mass = 70.0f;
        int32       MaxCollisionIterations = 8;
        bool        bCollideWithCharacters = true;

        FPhysicsCharacterHandle() = default;
        FPhysicsCharacterHandle(const FPhysicsCharacterHandle&) = delete;
        FPhysicsCharacterHandle& operator=(const FPhysicsCharacterHandle&) = delete;

        ~FPhysicsCharacterHandle()
        {
            if (b3World_IsValid(WorldId) && b3Body_IsValid(ProxyBody))
            {
                b3DestroyBody(ProxyBody);
            }
        }

        // HalfHeight is the cylinder half, matching SCapsuleColliderComponent, so the total is 2*(HalfHeight+Radius).
        b3Capsule MakeCapsule(float InRadius) const
        {
            const b3Vec3 Center{ TranslationOffset.x, TranslationOffset.y, TranslationOffset.z };
            return b3Capsule{ b3Vec3{ Center.x, Center.y - HalfHeight, Center.z },
                              b3Vec3{ Center.x, Center.y + HalfHeight, Center.z }, InRadius };
        }

        // Padding is a skin on the swept shape only, so other bodies and queries still see the authored capsule.
        b3Capsule MakeMoverCapsule() const { return MakeCapsule(Radius + Math::Max(Padding, 0.0f)); }
        b3Capsule MakeBodyCapsule() const  { return MakeCapsule(Radius); }
    };
}
