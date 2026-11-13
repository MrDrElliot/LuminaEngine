#include <gtest/gtest.h>

#include "Log/Log.h"
#include "UI/Tools/ConsoleLogEditorTool.h"

using namespace Lumina;

TEST(ConsoleLogFilterCache, SurvivesWrapAroundWithAnActiveFilter)
{
    const ELogLevel SavedLevel = Logging::GetLevel();
    Logging::SetLevel(ELogLevel::Trace);
    const uint32 SavedCapacity = Logging::GetConsoleLogQueueCapacity();

    Logging::SetConsoleLogQueueCapacity(300);
    Logging::Flush();
    Logging::ClearLogQueue();

    FConsoleFilter Filter;
    Filter.bShowTrace = Filter.bShowDebug = Filter.bShowInfo = Filter.bShowWarning = false;
    Filter.bShowError = Filter.bShowCritical = true;

    FConsoleVisibleRows Cache;

    for (int32 i = 0; i < 300; ++i)
    {
        LOG_WARN("filler {}", i);
    }
    Logging::Flush();

    const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();
    ASSERT_TRUE(Messages.IsFull());

    EXPECT_TRUE(Cache.Refresh(Messages, Filter));
    EXPECT_EQ(Cache.Indices.Num(), 0);

    // An idle frame with nothing changed must not rebuild.
    EXPECT_FALSE(Cache.Refresh(Messages, Filter));

    LOG_ERROR("overflow error one");
    Logging::Flush();
    ASSERT_TRUE(Messages.IsFull());
    EXPECT_TRUE(Cache.Refresh(Messages, Filter));
    ASSERT_EQ(Cache.Indices.Num(), 1);
    EXPECT_EQ(Messages[Cache.Indices.Last()].Message.View(), "overflow error one");

    LOG_ERROR("overflow error two");
    Logging::Flush();
    EXPECT_TRUE(Cache.Refresh(Messages, Filter));
    ASSERT_EQ(Cache.Indices.Num(), 2);
    EXPECT_EQ(Messages[Cache.Indices.Last()].Message.View(), "overflow error two");

    Logging::Flush();
    Logging::ClearLogQueue();
    Logging::SetConsoleLogQueueCapacity(SavedCapacity);
    Logging::SetLevel(SavedLevel);
}
