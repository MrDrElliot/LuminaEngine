#pragma once

#include "World/ECS/Registry.h"


#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "Box3DPhysicsScene.h"
#include "Box3DUtils.h"
#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"

namespace Lumina::Physics
{
    inline constexpr uint32 InvalidBodyHandle = 0xFFFFFFFFu;

    inline constexpr b3Transform IdentityTransform{ { 0.0f, 0.0f, 0.0f }, { { 0.0f, 0.0f, 0.0f }, 1.0f } };
    inline constexpr b3Vec3 UnitScale{ 1.0f, 1.0f, 1.0f };

    // Body user data carries the owning entity and the engine's stable body handle in one pointer-sized word.
    FORCEINLINE void* PackBodyUserData(ECS::FEntity Entity, uint32 Handle)
    {
        const uint64 Packed = (uint64)(Entity).Value | ((uint64)Handle << 32);
        return reinterpret_cast<void*>((uintptr_t)Packed);
    }

    FORCEINLINE ECS::FEntity UnpackEntity(void* UserData)
    {
        return (ECS::FEntity)(uint32)((uint64)reinterpret_cast<uintptr_t>(UserData) & 0xFFFFFFFFu);
    }

    FORCEINLINE uint32 UnpackHandle(void* UserData)
    {
        return (uint32)((uint64)reinterpret_cast<uintptr_t>(UserData) >> 32);
    }

    FORCEINLINE ECS::FEntity EntityOfBody(b3BodyId Body)
    {
        return b3Body_IsValid(Body) ? UnpackEntity(b3Body_GetUserData(Body)) : ECS::NullEntity;
    }

    FORCEINLINE uint32 HandleOfBody(b3BodyId Body)
    {
        return b3Body_IsValid(Body) ? UnpackHandle(b3Body_GetUserData(Body)) : InvalidBodyHandle;
    }

    FORCEINLINE ECS::FEntity EntityOfShape(b3ShapeId Shape)
    {
        return b3Shape_IsValid(Shape) ? EntityOfBody(b3Shape_GetBody(Shape)) : ECS::NullEntity;
    }

    FORCEINLINE FPendingShape MakeSphereShape(const FVector3& Center, float Radius)
    {
        FPendingShape Shape;
        Shape.Type = b3_sphereShape;
        Shape.Sphere = b3Sphere{ Box3DUtils::ToB3Vec3(Center), Radius };
        return Shape;
    }

    FORCEINLINE FPendingShape MakeCapsuleShape(const FVector3& Center, const FQuat& Rotation, float Radius, float HalfHeight)
    {
        const FVector3 Axis = Math::Rotate(Rotation, FVector3(0.0f, Math::Max(HalfHeight, 0.0f), 0.0f));

        FPendingShape Shape;
        Shape.Type = b3_capsuleShape;
        Shape.Capsule = b3Capsule{ Box3DUtils::ToB3Vec3(Center - Axis), Box3DUtils::ToB3Vec3(Center + Axis), Radius };
        return Shape;
    }

    FORCEINLINE FPendingShape MakeHullShape(const b3HullData* Hull, const FVector3& Offset, const FQuat& Rotation)
    {
        FPendingShape Shape;
        Shape.Type = b3_hullShape;
        Shape.Hull = Hull;
        Shape.Transform = Box3DUtils::ToB3Transform(Offset, Rotation);
        return Shape;
    }

    // Combine modes ride in the surface material id, which is the only per-shape data the mixing
    // callbacks receive. Bit 16 marks a real material so a plain zero id still means no material.
    inline constexpr uint64 MaterialIdPresentBit = 1ull << 16;

    FORCEINLINE uint64 PackMaterialId(uint8 FrictionCombine, uint8 RestitutionCombine)
    {
        return MaterialIdPresentBit | (uint64)FrictionCombine | ((uint64)RestitutionCombine << 8);
    }

    // Box3D's mover ignores initial overlap and cannot climb out of geometry, so a spawn that starts buried
    // has to be lifted onto the surface before the first step or it free falls forever.
    enum class EMoverSeatResult : uint8
    {
        Seated,
        Airborne,
        NoGeometry,
    };

    struct FSeatProbe
    {
        b3BodyId            IgnoreBody{};
        FCollisionProfile   Profile{};
        bool                bPermissive = true;
        b3Vec3              Point{};
        bool                bHit = false;
    };

    inline float SeatProbeCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3, float Fraction, uint64_t, int, int, void* Context)
    {
        FSeatProbe& Probe = *static_cast<FSeatProbe*>(Context);

        // Negative tells Box3D to drop this shape without clipping the ray, which is how the probe skips itself.
        if (B3_ID_EQUALS(b3Shape_GetBody(ShapeId), Probe.IgnoreBody))
        {
            return -1.0f;
        }

        if (!Box3DUtils::ShouldProfileCollideWithShape(Probe.Profile, ShapeId, Probe.bPermissive))
        {
            return -1.0f;
        }

        Probe.Point = Point;
        Probe.bHit = true;
        return Fraction;
    }

    inline EMoverSeatResult TrySeatMoverOnGround(b3WorldId WorldId, const b3Capsule& Mover, b3Vec3 Desired,
                                          b3QueryFilter Filter, const FCollisionProfile& Profile, float SearchDistance,
                                          b3BodyId IgnoreBody, b3Vec3& OutPosition)
    {
        OutPosition = Desired;

        // Measured off the capsule itself rather than assumed centered, since it can be offset onto the feet.
        const float BottomOffset = Mover.radius - Math::Min(Mover.center1.y, Mover.center2.y);
        const float TopOffset = Mover.radius + Math::Max(Mover.center1.y, Mover.center2.y);

        // Probing from a clear point above, since a ray that starts inside a surface reports nothing at all.
        const b3Vec3 Origin{ Desired.x + Mover.center1.x, Desired.y + TopOffset + SearchDistance, Desired.z + Mover.center1.z };

        // The character's own proxy is the first thing under this ray, so a closest-hit form finds only itself.
        FSeatProbe Probe;
        Probe.IgnoreBody = IgnoreBody;
        Probe.Profile = Profile;
        Probe.bPermissive = Box3DUtils::UsesPermissiveCollisionFilter();

        // A long reach separates a world with no collision yet from a deliberate spawn high in the air.
        constexpr float DeepReach = 5000.0f;
        b3World_CastRay(WorldId, Origin, b3Vec3{ 0.0f, -DeepReach, 0.0f }, Filter, &SeatProbeCallback, &Probe);
        if (!Probe.bHit)
        {
            return EMoverSeatResult::NoGeometry;
        }

        const float RestY = Probe.Point.y + BottomOffset;
        if (RestY <= Desired.y)
        {
            return EMoverSeatResult::Airborne;
        }

        OutPosition = b3Vec3{ Desired.x, RestY, Desired.z };
        return EMoverSeatResult::Seated;
    }

    void WarnTaperedCapsuleOnce();

    // Flattens a mesh resource's LOD-0 meshlets into positions, and triangles when the caller needs them.
    bool GatherMeshResourceGeometry(const FMeshResource& Resource, const FVector3& Scale,
                                    TVector<b3Vec3>& OutPositions, TVector<int32>* OutIndices);
}
