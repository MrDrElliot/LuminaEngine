#pragma once

#include <bit>
#include <random>
#include "Core/Assertions/Assert.h"
#include "Core/LuminaMacros.h"
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
#include "Core/Math/Random.h"

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
    
    // Signed-normalized 16-bit <-> float. -32768 maps to -1 like -32767 does, which is what makes the
    // encoding symmetric; the GPU's SNORM read does the same clamp.
    [[nodiscard]] constexpr float SNorm16ToFloat(int16 Value)
    {
        return Max((float)Value * (1.0f / 32767.0f), -1.0f);
    }

    [[nodiscard]] constexpr int16 FloatToSNorm16(float Value)
    {
        return (int16)(Clamp(Value, -1.0f, 1.0f) * 32767.0f + (Value >= 0.0f ? 0.5f : -0.5f));
    }

    [[nodiscard]] constexpr uint64 CountTrailingZeros64(uint64 Value)
    {
        return (uint64)std::countr_zero(Value);
    }

    template<std::integral T>
    [[nodiscard]] constexpr bool IsEven(T Val)
    {
        return ((Val) & 1) == 0;
    }


    [[nodiscard]] inline FQuat FindLookAtRotation(const FVector3& Target, const FVector3& From)
    {
        const FVector3 ForwardDirection = Normalize(Target - From);
        return QuatLookAt(ForwardDirection, FVector3(0.0f, 1.0f, 0.0f));
    }
}
