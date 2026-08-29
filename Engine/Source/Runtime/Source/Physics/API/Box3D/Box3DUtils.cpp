#include "RuntimePCH.h"
#include "Box3DUtils.h"
#include "Core/Console/ConsoleVariable.h"

namespace Lumina::Box3DUtils
{
    static TConsoleVar CVarStrictCollisionFilter("Physics.StrictCollisionFilter", false,
        "Require both sides of a pair to accept, which is Box3D's native rule and skips the filter callback. "
        "Off keeps the engine's historical rule where either side accepting is enough.");

    bool UsesPermissiveCollisionFilter()
    {
        return !CVarStrictCollisionFilter.GetValue();
    }

    b3Filter MakeShapeFilter(const FCollisionProfile& Profile, int32 GroupIndex)
    {
        b3Filter Filter = b3DefaultFilter();
        Filter.categoryBits = (uint64)Profile.Layer;

        // A widened mask lets a pair that only one side accepts still reach the callback that decides it.
        Filter.maskBits = UsesPermissiveCollisionFilter() ? B3_DEFAULT_MASK_BITS : (uint64)Profile.Mask;
        Filter.groupIndex = GroupIndex;
        return Filter;
    }

    // Marks the kinematic capsule standing in for a character mover, which is the one shape another
    // character may be told to ignore.
    static constexpr uint64 CharacterProxyBit = 1ull << 32;

    void* PackProfileUserData(const FCollisionProfile& Profile, bool bCharacterProxy)
    {
        uint64 Packed = (uint64)Profile.Layer | ((uint64)Profile.Mask << 16);
        if (bCharacterProxy)
        {
            Packed |= CharacterProxyBit;
        }
        return reinterpret_cast<void*>((uintptr_t)Packed);
    }

    bool IsCharacterProxyUserData(void* UserData)
    {
        return ((uint64)reinterpret_cast<uintptr_t>(UserData) & CharacterProxyBit) != 0;
    }

    b3QueryFilter MakeQueryFilter(const FCollisionProfile& Profile)
    {
        b3QueryFilter Filter = b3DefaultQueryFilter();
        Filter.categoryBits = (uint64)Profile.Layer;

        // A narrow mask would hide every pair that only the other side accepts, which contacts still take.
        Filter.maskBits = UsesPermissiveCollisionFilter() ? B3_DEFAULT_MASK_BITS : (uint64)Profile.Mask;
        return Filter;
    }

    b3BodyType ToBox3DBodyType(EBodyType Type)
    {
        switch (Type)
        {
            case EBodyType::Static:    return b3_staticBody;
            case EBodyType::Kinematic: return b3_kinematicBody;
            case EBodyType::Dynamic:   return b3_dynamicBody;
        }

        UNREACHABLE();
    }

    EBodyType FromBox3DBodyType(b3BodyType Type)
    {
        switch (Type)
        {
            case b3_staticBody:    return EBodyType::Static;
            case b3_kinematicBody: return EBodyType::Kinematic;
            case b3_dynamicBody:   return EBodyType::Dynamic;
            default:               return EBodyType::Static;
        }
    }

    b3Transform ToB3Transform(const FMatrix4& Mat)
    {
        FVector3 Scale;
        FQuat Rotation;
        FVector3 Translation;
        Math::Decompose(Mat, Scale, Rotation, Translation);
        return b3Transform{ ToB3Vec3(Translation), ToB3Quat(Rotation) };
    }

    FMatrix4 FromB3Transform(const b3Transform& Transform)
    {
        FMatrix4 Result = Math::ToMatrix4(FromB3Quat(Transform.q));
        Result[3][0] = Transform.p.x;
        Result[3][1] = Transform.p.y;
        Result[3][2] = Transform.p.z;
        Result[3][3] = 1.0f;
        return Result;
    }
}
