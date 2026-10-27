#include <gtest/gtest.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"

using namespace Lumina;

namespace
{
    SParticleBurst MakeBurst(float Time, int32 Count, int32 Repeats = 0, float Interval = 0.5f)
    {
        SParticleBurst Burst;
        Burst.Time = Time;
        Burst.Count = Count;
        Burst.Repeats = Repeats;
        Burst.Interval = Interval;
        return Burst;
    }

    // Steps a clock through one or more cycles and totals what the bursts release, the way the simulation planner does.
    uint32 RunClock(TSpan<const SParticleBurst> Bursts, float Step, int32 Steps, float CycleLength)
    {
        float Age = 0.0f;
        uint32 Total = 0;
        for (int32 Index = 0; Index < Steps; ++Index)
        {
            const float Prev = Age;
            Age += Step;
            if (CycleLength > 0.0f && Age >= CycleLength)
            {
                Age = fmodf(Age, CycleLength);
            }
            Total += CountBurstSpawns(Bursts, Prev, Age, CycleLength);
        }
        return Total;
    }
}

TEST(ParticleBurst, ABurstAtTheStartFiresOnTheFirstStep)
{
    const SParticleBurst Bursts[] = { MakeBurst(0.0f, 7) };
    EXPECT_EQ(CountBurstSpawns(Bursts, 0.0f, 1.0f / 60.0f, 2.0f), 7u);
    EXPECT_EQ(CountBurstSpawns(Bursts, 1.0f / 60.0f, 2.0f / 60.0f, 2.0f), 0u);
}

TEST(ParticleBurst, EachRepeatFiresOncePerCycle)
{
    const SParticleBurst Bursts[] = { MakeBurst(0.25f, 5, 3, 0.25f) };

    // Fires at 0.25, 0.5, 0.75 and 1.0 in a two second cycle, run for two cycles at 60 steps a second.
    EXPECT_EQ(RunClock(Bursts, 1.0f / 60.0f, 240, 2.0f), 5u * 4u * 2u);
}

TEST(ParticleBurst, RepeatsPastTheCycleEndAreDropped)
{
    const SParticleBurst Bursts[] = { MakeBurst(0.5f, 1, 10, 0.5f) };

    // Only 0.5 and 1.0 and 1.5 land inside a two second cycle.
    EXPECT_EQ(RunClock(Bursts, 1.0f / 30.0f, 60, 2.0f), 3u);
}

TEST(ParticleBurst, AWrappedStepStillFiresTheBurstAtTheCycleStart)
{
    const SParticleBurst Bursts[] = { MakeBurst(0.0f, 4), MakeBurst(1.95f, 2) };

    // From 1.9 the clock wraps to 0.05, which passes both 1.95 and the start of the next cycle.
    EXPECT_EQ(CountBurstSpawns(Bursts, 1.9f, 0.05f, 2.0f), 6u);
}

TEST(ParticleBurst, ANonCyclingClockFiresEachBurstOnce)
{
    const SParticleBurst Bursts[] = { MakeBurst(1.0f, 3, 2, 1.0f) };
    EXPECT_EQ(RunClock(Bursts, 0.1f, 100, 0.0f), 9u);
}
