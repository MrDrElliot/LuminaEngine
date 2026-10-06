using System;
using System.Runtime.CompilerServices;

namespace Lumina;

// Scalar math helpers and engine constants that complement System.MathF. Mirrors the gap-fillers from C++
// Core/Math (Scalar.h): clamping, interpolation, angle helpers, and the engine tolerance constants.
public static class Mathf
{
    public const float Pi = 3.14159265358979323846f;
    public const float TwoPi = 2.0f * Pi;
    public const float HalfPi = 0.5f * Pi;
    public const float InvPi = 1.0f / Pi;

    public const float Deg2Rad = Pi / 180.0f;
    public const float Rad2Deg = 180.0f / Pi;

    // Machine epsilon for float (matches C++ Math::Epsilon<float>).
    public const float Epsilon = 1.1920929e-07f;

    // LE_SMALL_NUMBER.
    public const float SmallNumber = 1e-8f;

    // LE_KINDA_SMALL_NUMBER.
    public const float KindaSmallNumber = 1e-4f;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Radians(float Degrees) => Degrees * Deg2Rad;
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Degrees(float Radians) => Radians * Rad2Deg;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Clamp(float V, float Lo, float Hi) => V < Lo ? Lo : (V > Hi ? Hi : V);
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Clamp01(float V) => V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V);

    // Alias for Clamp01 (GLSL/HLSL naming).
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Saturate(float V) => Clamp01(V);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Lerp(float A, float B, float T) => A + (B - A) * T;
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float LerpClamped(float A, float B, float T) => A + (B - A) * Clamp01(T);

    // Inverse of Lerp: the t that produces Value between A and B.
    public static float InverseLerp(float A, float B, float Value)
    {
        float Denom = B - A;
        return MathF.Abs(Denom) < SmallNumber ? 0.0f : Clamp01((Value - A) / Denom);
    }

    // Remaps Value from [InMin, InMax] to [OutMin, OutMax].
    public static float Remap(float Value, float InMin, float InMax, float OutMin, float OutMax)
    {
        return Lerp(OutMin, OutMax, InverseLerp(InMin, InMax, Value));
    }

    // Smooth Hermite interpolation between two edges.
    public static float SmoothStep(float Edge0, float Edge1, float X)
    {
        float T = Clamp01((X - Edge0) / (Edge1 - Edge0));
        return T * T * (3.0f - 2.0f * T);
    }

    // 0 if X < Edge, else 1.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Step(float Edge, float X) => X < Edge ? 0.0f : 1.0f;

    // Wraps T into [0, Length).
    public static float Repeat(float T, float Length) => Clamp(T - MathF.Floor(T / Length) * Length, 0.0f, Length);

    // Ping-pongs T between 0 and Length.
    public static float PingPong(float T, float Length)
    {
        T = Repeat(T, Length * 2.0f);
        return Length - MathF.Abs(T - Length);
    }

    // Floating-point modulo (result follows the sign of Y, like GLSL mod).
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Mod(float X, float Y) => X - Y * MathF.Floor(X / Y);

    // Fractional part.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Fract(float V) => V - MathF.Floor(V);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Sign(float V) => V > 0.0f ? 1.0f : (V < 0.0f ? -1.0f : 0.0f);

    // Eases Current toward Target at Rate per second, the same however the frames fall, unlike a lerp by a fixed fraction.
    public static float Damp(float Current, float Target, float Rate, float DeltaTime)
    {
        return Current + (Target - Current) * (1.0f - MathF.Exp(-Rate * DeltaTime));
    }

    // The heading of Direction in degrees, zero along +Z and growing toward +X, which is how a yaw input reads.
    public static float YawDegrees(FVector3 Direction) => MathF.Atan2(Direction.X, Direction.Z) * Rad2Deg;

    // The pitch of Direction in degrees as a camera reads it, positive looking down.
    public static float PitchDegrees(FVector3 Direction)
    {
        float Flat = MathF.Sqrt(Direction.X * Direction.X + Direction.Z * Direction.Z);
        return -MathF.Atan2(Direction.Y, Flat) * Rad2Deg;
    }

    // Moves Current toward Target by at most MaxDelta.
    public static float MoveTowards(float Current, float Target, float MaxDelta)
    {
        return MathF.Abs(Target - Current) <= MaxDelta ? Target : Current + Sign(Target - Current) * MaxDelta;
    }

    // Shortest signed difference between two angles (radians), in [-Pi, Pi].
    public static float DeltaAngle(float Current, float Target)
    {
        float Delta = Repeat(Target - Current, TwoPi);
        return Delta > Pi ? Delta - TwoPi : Delta;
    }

    // Interpolates between angles (radians) along the shortest path.
    public static float LerpAngle(float A, float B, float T)
    {
        return A + DeltaAngle(A, B) * Clamp01(T);
    }

    // Moves an angle (radians) toward a target along the shortest path.
    public static float MoveTowardsAngle(float Current, float Target, float MaxDelta)
    {
        float Delta = DeltaAngle(Current, Target);
        if (-MaxDelta < Delta && Delta < MaxDelta)
        {
            return Target;
        }
        return MoveTowards(Current, Current + Delta, MaxDelta);
    }

    // Degree twins of the above, for yaw and pitch, which gameplay keeps in degrees.
    public static float DeltaAngleDegrees(float Current, float Target)
    {
        float Delta = Repeat(Target - Current, 360.0f);
        return Delta > 180.0f ? Delta - 360.0f : Delta;
    }

    public static float LerpAngleDegrees(float A, float B, float T)
    {
        return A + DeltaAngleDegrees(A, B) * Clamp01(T);
    }

    public static float MoveTowardsAngleDegrees(float Current, float Target, float MaxDelta)
    {
        float Delta = DeltaAngleDegrees(Current, Target);
        if (-MaxDelta < Delta && Delta < MaxDelta)
        {
            return Target;
        }
        return MoveTowards(Current, Current + Delta, MaxDelta);
    }

    // Relative-tolerance compare scaled to magnitude (Unity-style).
    public static bool Approximately(float A, float B)
    {
        return MathF.Abs(B - A) <= MathF.Max(1e-6f * MathF.Max(MathF.Abs(A), MathF.Abs(B)), Epsilon * 8.0f);
    }

    public static bool IsNearlyZero(float V, float Tolerance = KindaSmallNumber) => MathF.Abs(V) <= Tolerance;
    public static bool IsNearlyEqual(float A, float B, float Tolerance = KindaSmallNumber) => MathF.Abs(A - B) <= Tolerance;

    // 1 / sqrt(V).
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float InvSqrt(float V) => 1.0f / MathF.Sqrt(V);

    // Smallest power of two >= V.
    public static int NextPowerOfTwo(int V)
    {
        if (V <= 1)
        {
            return 1;
        }
        V--;
        V |= V >> 1;
        V |= V >> 2;
        V |= V >> 4;
        V |= V >> 8;
        V |= V >> 16;
        return V + 1;
    }

    public static bool IsPowerOfTwo(int V) => V > 0 && (V & (V - 1)) == 0;
}
