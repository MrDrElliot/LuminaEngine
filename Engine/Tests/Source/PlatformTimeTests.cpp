#include <gtest/gtest.h>

#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaPlatformTimeTests
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::int64;
    using Lumina::uint64;

    TEST(PlatformTime, CyclesNeverGoBackwards)
    {
        uint64 Previous = PlatformTime::Cycles();
        for (int Index = 0; Index < 10000; ++Index)
        {
            const uint64 Now = PlatformTime::Cycles();
            ASSERT_GE(Now, Previous);
            Previous = Now;
        }
    }

    TEST(PlatformTime, SecondsAgreesWithTheCycleCounter)
    {
        const uint64 StartCycles = PlatformTime::Cycles();
        const double StartSeconds = PlatformTime::Seconds();

        PlatformTime::SleepMilliseconds(20);

        const double BySeconds = PlatformTime::Seconds() - StartSeconds;
        const double ByCycles = PlatformTime::ToSeconds(PlatformTime::Cycles() - StartCycles);

        EXPECT_NEAR(BySeconds, ByCycles, 0.002);
    }

    TEST(PlatformTime, UnitConversionsAreConsistent)
    {
        const uint64 Delta = static_cast<uint64>(1.0 / PlatformTime::SecondsPerCycle());

        EXPECT_NEAR(PlatformTime::ToSeconds(Delta), 1.0, 1e-6);
        EXPECT_NEAR(PlatformTime::ToMilliseconds(Delta), 1000.0, 1e-3);
        EXPECT_NEAR(PlatformTime::ToMicroseconds(Delta), 1000000.0, 1.0);
    }

    TEST(PlatformTime, SleepWaitsAtLeastAsLongAsAsked)
    {
        const PlatformTime::FStopwatch Timer;
        PlatformTime::SleepMilliseconds(50);
        const double Elapsed = Timer.ElapsedMilliseconds();

        EXPECT_GE(Elapsed, 45.0);
        EXPECT_LT(Elapsed, 500.0);
    }

    TEST(PlatformTime, YieldingIsCheapAndReturns)
    {
        for (int Index = 0; Index < 100; ++Index)
        {
            PlatformTime::YieldThread();
        }

        SUCCEED();
    }

    TEST(PlatformTime, WallClockSitsInThisCentury)
    {
        const int64 Nanoseconds = PlatformTime::UtcNanoseconds();
        const int64 Seconds = PlatformTime::UtcSeconds();

        // 2020-01-01 and 2100-01-01 as Unix seconds.
        EXPECT_GT(Seconds, 1577836800ll);
        EXPECT_LT(Seconds, 4102444800ll);
        EXPECT_EQ(Seconds, Nanoseconds / 1000000000ll);
    }

    TEST(PlatformTime, WallClockAdvances)
    {
        const int64 First = PlatformTime::UtcNanoseconds();
        PlatformTime::SleepMilliseconds(20);
        const int64 Second = PlatformTime::UtcNanoseconds();

        EXPECT_GT(Second, First);
    }

    TEST(PlatformTime, BreaksDownAKnownInstant)
    {
        // 2001-09-09T01:46:40Z, the moment Unix time hit 1,000,000,000.
        const int64 Nanoseconds = 1000000000ll * 1000000000ll;
        const PlatformTime::FDateTime Utc = PlatformTime::UtcTime(Nanoseconds);

        EXPECT_EQ(Utc.Year, 2001);
        EXPECT_EQ(Utc.Month, 9);
        EXPECT_EQ(Utc.Day, 9);
        EXPECT_EQ(Utc.Hour, 1);
        EXPECT_EQ(Utc.Minute, 46);
        EXPECT_EQ(Utc.Second, 40);
        EXPECT_EQ(Utc.Millisecond, 0);
        EXPECT_EQ(Utc.DayOfWeek, 0);
    }

    TEST(PlatformTime, KeepsSubSecondPrecisionInTheBreakdown)
    {
        const int64 Nanoseconds = 1000000000ll * 1000000000ll + 250000000ll;
        const PlatformTime::FDateTime Utc = PlatformTime::UtcTime(Nanoseconds);

        EXPECT_EQ(Utc.Second, 40);
        EXPECT_EQ(Utc.Millisecond, 250);
    }

    TEST(PlatformTime, LocalNowMatchesTheWallClockDate)
    {
        const PlatformTime::FDateTime Now = PlatformTime::LocalNow();

        EXPECT_GE(Now.Year, 2020);
        EXPECT_GE(Now.Month, 1);
        EXPECT_LE(Now.Month, 12);
        EXPECT_GE(Now.Day, 1);
        EXPECT_LE(Now.Day, 31);
        EXPECT_LE(Now.Hour, 23);
        EXPECT_LE(Now.Minute, 59);
        EXPECT_LE(Now.Second, 60);
    }

    TEST(PlatformTime, StopwatchRestartsFromZero)
    {
        PlatformTime::FStopwatch Timer;
        PlatformTime::SleepMilliseconds(20);
        EXPECT_GE(Timer.ElapsedMilliseconds(), 15.0);

        Timer.Restart();
        EXPECT_LT(Timer.ElapsedMilliseconds(), 15.0);
    }
}
