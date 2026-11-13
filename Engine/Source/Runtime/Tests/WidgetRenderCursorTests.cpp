#include <gtest/gtest.h>

#include "UI/WorldUIContext.h"

using namespace Lumina;

TEST(WidgetRenderCursor, RotatesIndependentlyPerWorld)
{
    FWorldUIContext WorldA;
    FWorldUIContext WorldB;
    constexpr uint32 JobCount = 3;

    EXPECT_EQ(WorldA.NextWidgetRenderStart(JobCount), 0u);
    EXPECT_EQ(WorldA.NextWidgetRenderStart(JobCount), 1u);

    // World A rendering earlier in the frame must not shift where World B starts.
    EXPECT_EQ(WorldB.NextWidgetRenderStart(JobCount), 0u);
    EXPECT_EQ(WorldA.NextWidgetRenderStart(JobCount), 2u);
    EXPECT_EQ(WorldA.NextWidgetRenderStart(JobCount), 0u);
    EXPECT_EQ(WorldB.NextWidgetRenderStart(JobCount), 1u);
}

TEST(WidgetRenderCursor, ClampsWhenJobCountShrinks)
{
    FWorldUIContext World;
    World.NextWidgetRenderStart(5);
    World.NextWidgetRenderStart(5);
    World.NextWidgetRenderStart(5);

    EXPECT_EQ(World.NextWidgetRenderStart(2), 1u);
    EXPECT_EQ(World.NextWidgetRenderStart(2), 0u);
}
