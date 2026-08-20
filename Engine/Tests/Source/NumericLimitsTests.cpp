#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "Containers/String.h"
#include "Core/Math/MathString.h"
#include "Core/Math/Scalar.h"
#include "Core/Templates/NumericLimits.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaNumericLimitsTests
{
    using Lumina::FString;
    using Lumina::FVector2;
    using Lumina::FVector3;
    using Lumina::FVector4;
    using Lumina::FQuat;
    using Lumina::int8;
    using Lumina::int16;
    using Lumina::int32;
    using Lumina::int64;
    using Lumina::uint8;
    using Lumina::uint16;
    using Lumina::uint32;
    using Lumina::uint64;

    // The standard is the reference for every value we hand-wrote.
    #define EXPECT_LIMIT_MATCHES(Type)                                                          \
        EXPECT_EQ(TNumericLimits<Type>::Min(), std::numeric_limits<Type>::min());               \
        EXPECT_EQ(TNumericLimits<Type>::Lowest(), std::numeric_limits<Type>::lowest());         \
        EXPECT_EQ(TNumericLimits<Type>::Max(), std::numeric_limits<Type>::max())

    TEST(NumericLimits, IntegerBoundsMatchTheStandard)
    {
        EXPECT_LIMIT_MATCHES(uint8);
        EXPECT_LIMIT_MATCHES(uint16);
        EXPECT_LIMIT_MATCHES(uint32);
        EXPECT_LIMIT_MATCHES(uint64);
        EXPECT_LIMIT_MATCHES(int8);
        EXPECT_LIMIT_MATCHES(int16);
        EXPECT_LIMIT_MATCHES(int32);
        EXPECT_LIMIT_MATCHES(int64);
    }

    TEST(NumericLimits, FloatBoundsMatchTheStandard)
    {
        EXPECT_LIMIT_MATCHES(float);
        EXPECT_LIMIT_MATCHES(double);

        EXPECT_EQ(TNumericLimits<float>::Epsilon(), std::numeric_limits<float>::epsilon());
        EXPECT_EQ(TNumericLimits<double>::Epsilon(), std::numeric_limits<double>::epsilon());
    }

    TEST(NumericLimits, InfinityAndNaNHaveTheRightBitPatterns)
    {
        EXPECT_TRUE(std::isinf(TNumericLimits<float>::Infinity()));
        EXPECT_GT(TNumericLimits<float>::Infinity(), 0.0f);
        EXPECT_TRUE(std::isinf(-TNumericLimits<float>::Infinity()));
        EXPECT_LT(-TNumericLimits<float>::Infinity(), 0.0f);

        EXPECT_TRUE(std::isinf(TNumericLimits<double>::Infinity()));
        EXPECT_GT(TNumericLimits<double>::Infinity(), 0.0);

        EXPECT_TRUE(std::isnan(TNumericLimits<float>::QuietNaN()));
        EXPECT_TRUE(std::isnan(TNumericLimits<double>::QuietNaN()));

        EXPECT_EQ(TNumericLimits<float>::Infinity(), std::numeric_limits<float>::infinity());
        EXPECT_EQ(TNumericLimits<double>::Infinity(), std::numeric_limits<double>::infinity());
    }

    TEST(NumericLimits, EveryMemberIsAConstantExpression)
    {
        static_assert(TNumericLimits<int32>::Max() == 2147483647);
        static_assert(TNumericLimits<uint64>::Max() == 18446744073709551615ull);
        static_assert(TNumericLimits<float>::Epsilon() > 0.0f);
        static_assert(TNumericLimits<float>::Infinity() > TNumericLimits<float>::Max());
        static_assert(TNumericLimits<double>::Infinity() > TNumericLimits<double>::Max());
        SUCCEED();
    }

    TEST(NumericLimits, CvQualifiersFallThroughToTheUnderlyingType)
    {
        EXPECT_EQ(TNumericLimits<const int32>::Max(), TNumericLimits<int32>::Max());
        EXPECT_EQ(TNumericLimits<volatile float>::Epsilon(), TNumericLimits<float>::Epsilon());
    }

    TEST(NumericLimits, ScalarEpsilonRoutesThroughIt)
    {
        EXPECT_EQ(Lumina::Math::Epsilon<float>(), TNumericLimits<float>::Epsilon());
        EXPECT_EQ(Lumina::Math::Epsilon<double>(), TNumericLimits<double>::Epsilon());
    }

    TEST(MathToString, RendersTheMathTypesWithoutTrailingZeroNoise)
    {
        EXPECT_EQ(Lumina::Math::ToString(FVector2(1.0f, -2.5f)), "(1, -2.5)");
        EXPECT_EQ(Lumina::Math::ToString(FVector3(0.0f, 0.5f, 3.0f)), "(0, 0.5, 3)");
        EXPECT_EQ(Lumina::Math::ToString(FVector4(1.0f, 2.0f, 3.0f, 4.0f)), "(1, 2, 3, 4)");
    }

    TEST(MathToString, KeepsPrecisionThatFixedNotationWouldHaveLost)
    {
        // std::to_string would have rendered this as "0.000000".
        const FString Text = Lumina::Math::ToString(FVector2(1e-8f, 1.0f));

        EXPECT_NE(Text, "(0.000000, 1.000000)");
        EXPECT_NE(Text.find("e-08"), FString::npos);
    }

    TEST(MathToString, RendersAQuaternionWithItsWFirst)
    {
        const FQuat Rotation(1.0f, 0.0f, 0.0f, 0.0f);
        EXPECT_EQ(Lumina::Math::ToString(Rotation), "(w=1, 0, 0, 0)");
    }
}
