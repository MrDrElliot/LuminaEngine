#pragma once

#include <bit>
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

    [[nodiscard]] constexpr bool IsNearlyEqual(float LHS, float RHS, float Epsilon = Math::kKindaSmallNumber)
    {
        return Abs(LHS - RHS) <= Epsilon;
    }

    [[nodiscard]] constexpr bool IsNearlyZero(float Value, float Epsilon = Math::kKindaSmallNumber)
    {
        return Abs(Value) <= Epsilon;
    }
    
    // -32768 maps to -1 like -32767 does, matching the GPU's SNORM read.
    [[nodiscard]] constexpr float SNorm16ToFloat(int16 Value)
    {
        return Max((float)Value * (1.0f / 32767.0f), -1.0f);
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

    // Eases Current toward Target at Rate per second, the same at any frame rate. Mirrors LuminaSharp's Mathf.Damp.
    [[nodiscard]] inline float Damp(float Current, float Target, float Rate, float DeltaTime)
    {
        return Current + (Target - Current) * (1.0f - std::exp(-Rate * DeltaTime));
    }

    // Moves Current toward Target by at most MaxDelta.
    [[nodiscard]] inline float MoveTowards(float Current, float Target, float MaxDelta)
    {
        return Abs(Target - Current) <= MaxDelta ? Target : Current + (Target > Current ? MaxDelta : -MaxDelta);
    }

    // The shortest signed turn from Current to Target in degrees, within [-180, 180].
    [[nodiscard]] inline float DeltaAngleDegrees(float Current, float Target)
    {
        float Delta = std::fmod(Target - Current, 360.0f);
        if (Delta < 0.0f)
        {
            Delta += 360.0f;
        }
        return Delta > 180.0f ? Delta - 360.0f : Delta;
    }

    // Interpolates between two angles in degrees along the shorter way round.
    [[nodiscard]] inline float LerpAngleDegrees(float A, float B, float Alpha)
    {
        return A + DeltaAngleDegrees(A, B) * Saturate(Alpha);
    }

    // Turns Current toward Target in degrees by at most MaxDelta, along the shorter way round.
    [[nodiscard]] inline float MoveTowardsAngleDegrees(float Current, float Target, float MaxDelta)
    {
        const float Delta = DeltaAngleDegrees(Current, Target);
        return Abs(Delta) < MaxDelta ? Target : MoveTowards(Current, Current + Delta, MaxDelta);
    }

    // The heading of Direction in degrees, zero along +Z and growing toward +X, which is how a yaw input reads.
    [[nodiscard]] inline float YawDegrees(const FVector3& Direction)
    {
        return Degrees(std::atan2(Direction.x, Direction.z));
    }

    // The pitch of Direction in degrees as a camera reads it, positive looking down.
    [[nodiscard]] inline float PitchDegrees(const FVector3& Direction)
    {
        const float Across = std::sqrt(Direction.x * Direction.x + Direction.z * Direction.z);
        return -Degrees(std::atan2(Direction.y, Across));
    }

    // A with its height dropped.
    [[nodiscard]] inline FVector3 Flat(const FVector3& A)
    {
        return FVector3(A.x, 0.0f, A.z);
    }

    // The distance between A and B across the ground, ignoring height.
    [[nodiscard]] inline float FlatDistance(const FVector3& A, const FVector3& B)
    {
        const float Dx = A.x - B.x;
        const float Dz = A.z - B.z;
        return std::sqrt(Dx * Dx + Dz * Dz);
    }

    // A turn of Yaw degrees about +Y, the rotation a yaw input describes.
    [[nodiscard]] inline FQuat FromYawDegrees(float Yaw)
    {
        return FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), Radians(Yaw));
    }
}
