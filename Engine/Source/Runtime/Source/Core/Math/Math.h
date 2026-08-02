#pragma once

#include <random>
#include "Core/Assertions/Assert.h"
#include <eastl/type_traits.h>
#include "Core/LuminaMacros.h"
#include "eastl/utility.h"
#include "Platform/GenericPlatform.h"

// The Lumina math hub. Pulls in the in-house vector/quat/matrix library and adds
// the scalar utilities that don't belong to a single type.
#include "Core/Math/Scalar.h"
#include "Core/Math/Vector/Vector.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Matrix/Matrix.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Core/Math/Packing.h"
#include "Core/Math/MathString.h"

namespace Lumina::Math
{
    [[nodiscard]] constexpr int NextPowerOfTwo(int v)
    {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        return v;
    }

    template <typename T>
    [[nodiscard]] constexpr T AlignUp(T InV, uint64 InAlignment)
    {
        return T((static_cast<uint64>(InV) + InAlignment - 1) & ~(InAlignment - 1));
    }

    // Generic linear interpolation for any type with +, - and *scalar (scalars,
    // FColor, ...). Vectors resolve to the more specialized overload in VectorMath.h.
    template<typename T>
    [[nodiscard]] constexpr T Lerp(const T& A, const T& B, float Alpha)
    {
        return A + (B - A) * Alpha;
    }

    [[nodiscard]] constexpr bool IsNearlyEqual(float LHS, float RHS, float Epsilon = LE_KINDA_SMALL_NUMBER)
    {
        return Abs(LHS - RHS) <= Epsilon;
    }

    [[nodiscard]] constexpr bool IsNearlyZero(float Value, float Epsilon = LE_KINDA_SMALL_NUMBER)
    {
        return Abs(Value) <= Epsilon;
    }

    [[nodiscard]] constexpr uint64 CountTrailingZeros64(uint64 Value)
    {
        if (Value == 0)
        {
            return 64;
        }
        uint64 Result = 0;
        while ((Value & 1) == 0)
        {
            Value >>= 1;
            ++Result;
        }
        return Result;
    }

    template<std::integral T>
    [[nodiscard]] constexpr bool IsEven(T Val)
    {
        return ((Val) & 1) == 0;
    }

    template<std::integral T>
    requires(eastl::is_unsigned_v<T> && (sizeof(T) <= 4))
    [[nodiscard]] T RandRange(T First, T Second)
    {
        if (First > Second)
        {
            eastl::swap(First, Second);
        }

        thread_local std::mt19937 Random([]()
        {
            std::random_device RD;
            std::seed_seq Seed{ RD(), RD(), RD(), RD(), RD(), RD(), RD(), RD() };
            return std::mt19937(Seed);
        }());

        std::uniform_int_distribution<uint32> Distribution(First, Second);

        return Distribution(Random);
    }

    [[nodiscard]] inline FQuat FindLookAtRotation(const FVector3& Target, const FVector3& From)
    {
        const FVector3 ForwardDirection = Normalize(Target - From);
        return QuatLookAt(ForwardDirection, FVector3(0.0f, 1.0f, 0.0f));
    }
}
