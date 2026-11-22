#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "Core/Math/Math.h"

using namespace Lumina;

namespace MathConstexprTestsInternal
{
    constexpr std::array<double, 12> kAngles = { 0.0, 0.1, -0.5, 1.0, 1.5707963267948966, 2.5, -3.0, 3.141592653589793, 6.0, -10.0, 42.0, 100.0 };
    constexpr std::array<double, 10> kPositives = { 1e-6, 0.01, 0.5, 1.0, 2.0, 3.75, 10.0, 1234.5, 1e10, 1e-200 };
    constexpr std::array<double, 9> kUnit = { -1.0, -0.99, -0.5, -0.1, 0.0, 0.25, 0.7, 0.999, 1.0 };

    template<size_t N, typename TFunc>
    constexpr std::array<double, N> MapAll(const std::array<double, N>& Inputs, TFunc Func)
    {
        std::array<double, N> Out{};
        for (size_t Index = 0; Index < N; ++Index)
        {
            Out[Index] = Func(Inputs[Index]);
        }
        return Out;
    }

    // Relative for large results and absolute near zero, since a reduced angle carries the reduction's rounding.
    void ExpectClose(const char* Name, double Input, double CompileTime, double RunTime)
    {
        const double Tolerance = 1e-12 * Math::Max(1.0, std::abs(RunTime));
        EXPECT_NEAR(CompileTime, RunTime, Tolerance) << Name << "(" << Input << ")";
    }
}

using namespace MathConstexprTestsInternal;

// The functions players reach for first, evaluated entirely at compile time.
static_assert(Math::Sqrt(16.0f) == 4.0f);
static_assert(Math::Floor(-1.5) == -2.0 && Math::Ceil(-1.5) == -1.0 && Math::Round(2.5) == 3.0 && Math::Trunc(-2.7) == -2.0);
static_assert(Math::FMod(-7.0, 3.0) == -1.0 && Math::Mod(-7.0, 3.0) == 2.0);
static_assert(Math::IsNearlyEqual(Math::Length(FVector3(3.0f, 4.0f, 0.0f)), 5.0f));
static_assert(Math::IsNearlyEqual(Math::Normalize(FVector3(0.0f, 0.0f, 2.0f)).z, 1.0f));
static_assert(Math::IsNearlyEqual(Math::FindLookAtRotation(FVector3(0.0f, 0.0f, 10.0f), FVector3(0.0f)).w, 1.0f));
static_assert(Math::IsNearlyEqual(Math::FromYawDegrees(90.0f).y, 0.70710678f));
static_assert(Math::IsNearlyEqual(Math::YawDegrees(FVector3(1.0f, 0.0f, 0.0f)), 90.0f));
static_assert(Math::IsNearlyEqual(Math::DeltaAngleDegrees(350.0f, 10.0f), 20.0f));
static_assert(Math::Inverse(Math::Translate(FMatrix4(1.0f), FVector3(1.0f, 2.0f, 3.0f)))[3][0] == -1.0f);
static_assert((Math::Translate(FMatrix4(1.0f), FVector3(1.0f, 2.0f, 3.0f)) * FVector4(0.0f, 0.0f, 0.0f, 1.0f)).z == 3.0f);

// The compile-time kernels only run during constant evaluation, so this holds them to the run-time library they stand in for.
TEST(MathConstexpr, CompileTimeKernelsMatchTheStandardLibrary)
{
    constexpr auto Sin = MapAll(kAngles, [](double V) { return Math::Sin(V); });
    constexpr auto Cos = MapAll(kAngles, [](double V) { return Math::Cos(V); });
    constexpr auto Tan = MapAll(kAngles, [](double V) { return Math::Tan(V); });
    constexpr auto Atan = MapAll(kAngles, [](double V) { return Math::Atan(V); });
    constexpr auto Atan2 = MapAll(kAngles, [](double V) { return Math::Atan2(V, 1.0 - V); });
    constexpr auto Exp = MapAll(kAngles, [](double V) { return Math::Exp(V); });
    constexpr auto Exp2 = MapAll(kAngles, [](double V) { return Math::Exp2(V); });
    constexpr auto Floor = MapAll(kAngles, [](double V) { return Math::Floor(V); });
    constexpr auto Round = MapAll(kAngles, [](double V) { return Math::Round(V); });
    constexpr auto FMod = MapAll(kAngles, [](double V) { return Math::FMod(V, 0.75); });
    constexpr auto Sqrt = MapAll(kPositives, [](double V) { return Math::Sqrt(V); });
    constexpr auto Log = MapAll(kPositives, [](double V) { return Math::Log(V); });
    constexpr auto Log2 = MapAll(kPositives, [](double V) { return Math::Log2(V); });
    constexpr auto Pow = MapAll(kPositives, [](double V) { return Math::Pow(V, 2.2); });
    constexpr auto Asin = MapAll(kUnit, [](double V) { return Math::Asin(V); });
    constexpr auto Acos = MapAll(kUnit, [](double V) { return Math::Acos(V); });

    for (size_t Index = 0; Index < kAngles.size(); ++Index)
    {
        const double V = kAngles[Index];
        ExpectClose("Sin", V, Sin[Index], std::sin(V));
        ExpectClose("Cos", V, Cos[Index], std::cos(V));
        ExpectClose("Tan", V, Tan[Index], std::tan(V));
        ExpectClose("Atan", V, Atan[Index], std::atan(V));
        ExpectClose("Atan2", V, Atan2[Index], std::atan2(V, 1.0 - V));
        ExpectClose("Exp", V, Exp[Index], std::exp(V));
        ExpectClose("Exp2", V, Exp2[Index], std::exp2(V));
        ExpectClose("Floor", V, Floor[Index], std::floor(V));
        ExpectClose("Round", V, Round[Index], std::round(V));
        ExpectClose("FMod", V, FMod[Index], std::fmod(V, 0.75));
    }
    for (size_t Index = 0; Index < kPositives.size(); ++Index)
    {
        const double V = kPositives[Index];
        ExpectClose("Sqrt", V, Sqrt[Index], std::sqrt(V));
        ExpectClose("Log", V, Log[Index], std::log(V));
        ExpectClose("Log2", V, Log2[Index], std::log2(V));
        ExpectClose("Pow", V, Pow[Index], std::pow(V, 2.2));
    }
    for (size_t Index = 0; Index < kUnit.size(); ++Index)
    {
        const double V = kUnit[Index];
        ExpectClose("Asin", V, Asin[Index], std::asin(V));
        ExpectClose("Acos", V, Acos[Index], std::acos(V));
    }
}

// A constant and a run-time call of the same function agree, so a value folded at compile time matches the one a frame computes.
TEST(MathConstexpr, ConstantAndRunTimeResultsAgree)
{
    constexpr FQuat Folded = Math::FindLookAtRotation(FVector3(3.0f, 1.0f, -2.0f), FVector3(0.5f, 0.0f, 0.0f));
    volatile float Spin = 3.0f;
    const FQuat Live = Math::FindLookAtRotation(FVector3(Spin, 1.0f, -2.0f), FVector3(0.5f, 0.0f, 0.0f));
    EXPECT_NEAR(Folded.w, Live.w, 1e-6f);
    EXPECT_NEAR(Folded.x, Live.x, 1e-6f);
    EXPECT_NEAR(Folded.y, Live.y, 1e-6f);
    EXPECT_NEAR(Folded.z, Live.z, 1e-6f);

    constexpr FMatrix4 FoldedInverse = Math::Inverse(Math::Rotate(FMatrix4(1.0f), 0.7f, FVector3(0.0f, 1.0f, 0.0f)));
    const FMatrix4 LiveInverse = Math::Inverse(Math::Rotate(FMatrix4(1.0f), (float)Spin * 0.7f / 3.0f, FVector3(0.0f, 1.0f, 0.0f)));
    for (int Column = 0; Column < 4; ++Column)
    {
        for (int Row = 0; Row < 4; ++Row)
        {
            EXPECT_NEAR(FoldedInverse[Column][Row], LiveInverse[Column][Row], 1e-6f);
        }
    }
}
