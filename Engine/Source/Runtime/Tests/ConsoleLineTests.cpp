#include <gtest/gtest.h>

#include "Core/Console/ConsoleVariable.h"

using namespace Lumina;

// Nothing here sets a variable, since a successful set writes the console config to disk.

TEST(ConsoleLine, EmptyAndBlankLinesDoNothing)
{
    EXPECT_EQ(FConsoleRegistry::Get().ExecuteLine("", false).Kind, EConsoleLineResult::Empty);
    EXPECT_EQ(FConsoleRegistry::Get().ExecuteLine("   \t ", false).Kind, EConsoleLineResult::Empty);
}

TEST(ConsoleLine, ReadsAVariableIgnoringSurroundingSpace)
{
    FConsoleRegistry& Registry = FConsoleRegistry::Get();
    const TOptional<FString> Expected = Registry.GetValueAsString("DebugDraw.MaxDistance");
    ASSERT_TRUE(Expected.IsSet());

    const FConsoleLineResult Result = Registry.ExecuteLine("  DebugDraw.MaxDistance  ", false);
    EXPECT_EQ(Result.Kind, EConsoleLineResult::ReadVariable);
    EXPECT_EQ(Result.Name, FString("DebugDraw.MaxDistance"));
    EXPECT_EQ(Result.Value, *Expected);
}

TEST(ConsoleLine, RejectsAValueOfTheWrongTypeAndKeepsTheOldOne)
{
    FConsoleRegistry& Registry = FConsoleRegistry::Get();
    const TOptional<FString> Before = Registry.GetValueAsString("DebugDraw.MaxDistance");

    const FConsoleLineResult Result = Registry.ExecuteLine("DebugDraw.MaxDistance not-a-number", false);
    EXPECT_EQ(Result.Kind, EConsoleLineResult::RejectedValue);
    EXPECT_EQ(Result.Value, FString("not-a-number"));
    EXPECT_EQ(Registry.GetValueAsString("DebugDraw.MaxDistance"), Before);
}

TEST(ConsoleLine, ReportsAnUnknownName)
{
    const FConsoleLineResult Result = FConsoleRegistry::Get().ExecuteLine("No.Such.Variable 3", false);
    EXPECT_EQ(Result.Kind, EConsoleLineResult::Unknown);
    EXPECT_EQ(Result.Name, FString("No.Such.Variable"));
}

TEST(ConsoleLine, RunsACommand)
{
    const FConsoleLineResult Result = FConsoleRegistry::Get().ExecuteLine("Mesh.GeometryStats", false);
    EXPECT_EQ(Result.Kind, EConsoleLineResult::RanCommand);
    EXPECT_EQ(Result.Name, FString("Mesh.GeometryStats"));
}
