#pragma once

#include <box3d/box3d.h>
#include "Physics/PhysicsTypes.h"
#include "Core/Math/Math.h"
#include "Core/Math/Matrix/MatrixMath.h"

namespace Lumina::Box3DUtils
{
    RUNTIME_API b3Filter MakeShapeFilter(const FCollisionProfile& Profile, int32 GroupIndex = 0);

    // Shape user data carries the authored profile, since the broad-phase mask is widened for the callback.
    RUNTIME_API void* PackProfileUserData(const FCollisionProfile& Profile, bool bCharacterProxy = false);
    RUNTIME_API bool IsCharacterProxyUserData(void* UserData);

    // True when the engine's historical rule applies, where either side accepting is enough.
    RUNTIME_API bool UsesPermissiveCollisionFilter();
    RUNTIME_API b3QueryFilter MakeQueryFilter(const FCollisionProfile& Profile);

    FORCEINLINE FCollisionProfile UnpackProfileUserData(void* UserData)
    {
        const uint64 Packed = (uint64)reinterpret_cast<uintptr_t>(UserData);

        FCollisionProfile Profile;
        Profile.Layer = (ECollisionProfiles)(uint16)(Packed & 0xFFFFull);
        Profile.Mask = (ECollisionProfiles)(uint16)((Packed >> 16) & 0xFFFFull);
        return Profile;
    }

    // Inline, since a shape id only means something to the image holding the box3d copy that made it.
    FORCEINLINE bool ShouldProfileCollideWithShape(const FCollisionProfile& Profile, b3ShapeId ShapeId, bool bPermissive)
    {
        const FCollisionProfile Other = UnpackProfileUserData(b3Shape_GetUserData(ShapeId));

        // Strict mode already applied the other half of the pair test through the query filter itself.
        return bPermissive ? Profile.ShouldCollide(Other) : (Profile.Mask & Other.Layer) != (ECollisionProfiles)0;
    }

    RUNTIME_API b3BodyType ToBox3DBodyType(EBodyType Type);
    RUNTIME_API EBodyType FromBox3DBodyType(b3BodyType Type);

    FORCEINLINE b3Vec3 ToB3Vec3(const FVector3& Vec)
    {
        return b3Vec3{ Vec.x, Vec.y, Vec.z };
    }

    FORCEINLINE FVector3 FromB3Vec3(const b3Vec3& Vec)
    {
        return FVector3(Vec.x, Vec.y, Vec.z);
    }

    FORCEINLINE b3Quat ToB3Quat(const FQuat& Quat)
    {
        return b3Quat{ b3Vec3{ Quat.x, Quat.y, Quat.z }, Quat.w };
    }

    FORCEINLINE FQuat FromB3Quat(const b3Quat& Quat)
    {
        return FQuat(Quat.s, Quat.v.x, Quat.v.y, Quat.v.z);
    }

    FORCEINLINE b3Transform ToB3Transform(const FVector3& Position, const FQuat& Rotation)
    {
        return b3Transform{ ToB3Vec3(Position), ToB3Quat(Rotation) };
    }

    RUNTIME_API b3Transform ToB3Transform(const FMatrix4& Mat);
    RUNTIME_API FMatrix4 FromB3Transform(const b3Transform& Transform);

    FORCEINLINE b3HexColor ToB3Color(const FVector4& Color)
    {
        const uint32 R = (uint32)Math::Clamp(Color.r * 255.0f, 0.0f, 255.0f);
        const uint32 G = (uint32)Math::Clamp(Color.g * 255.0f, 0.0f, 255.0f);
        const uint32 B = (uint32)Math::Clamp(Color.b * 255.0f, 0.0f, 255.0f);
        return (b3HexColor)((R << 16) | (G << 8) | B);
    }

    FORCEINLINE FVector4 FromB3Color(b3HexColor Color)
    {
        const uint32 Value = (uint32)Color;
        return FVector4(((Value >> 16) & 0xFF) / 255.0f, ((Value >> 8) & 0xFF) / 255.0f, (Value & 0xFF) / 255.0f, 1.0f);
    }
}
