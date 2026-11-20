namespace Lumina;

// The C++ SimMath, with the same constants and operation order, so a C# system computes what a C++ one does on every peer.
public static class SimMath
{
    public const float Pi = 3.14159265358979323846f;
    public const float HalfPi = 1.57079632679489661923f;
    public const float TwoPi = 6.28318530717958647692f;

    // Wraps to [-pi, pi] by subtracting whole turns, rounding the turn count the same way on every machine.
    public static float WrapAngle(float Radians)
    {
        float Turns = Radians * (1.0f / TwoPi);
        float Whole = (float)(long)(Turns + (Turns >= 0.0f ? 0.5f : -0.5f));
        return Radians - Whole * TwoPi;
    }

    public static float Sin(float Radians)
    {
        float X = WrapAngle(Radians);
        if (X > HalfPi)
        {
            X = Pi - X;
        }
        else if (X < -HalfPi)
        {
            X = -Pi - X;
        }
        float X2 = X * X;
        return X * (1.0f + X2 * (-1.6666667e-1f + X2 * (8.3333333e-3f + X2 * (-1.9841270e-4f + X2 * (2.7557319e-6f + X2 * -2.5052108e-8f)))));
    }

    public static float Cos(float Radians) => Sin(Radians + HalfPi);

    public static float Atan(float Value)
    {
        bool bNegative = Value < 0.0f;
        float A = bNegative ? -Value : Value;
        bool bInverted = A > 1.0f;
        float X = bInverted ? 1.0f / A : A;
        float X2 = X * X;
        float Result = X * (0.9998660f + X2 * (-0.3302995f + X2 * (0.1801410f + X2 * (-0.0851330f + X2 * 0.0208351f))));
        if (bInverted)
        {
            Result = HalfPi - Result;
        }
        return bNegative ? -Result : Result;
    }

    public static float Atan2(float Y, float X)
    {
        if (X > 0.0f)
        {
            return Atan(Y / X);
        }
        if (X < 0.0f)
        {
            return Atan(Y / X) + (Y >= 0.0f ? Pi : -Pi);
        }
        return Y > 0.0f ? HalfPi : (Y < 0.0f ? -HalfPi : 0.0f);
    }
}
