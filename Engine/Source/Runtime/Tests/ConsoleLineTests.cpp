#include <gtest/gtest.h>

#include "Core/Console/ConsoleVariable.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

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

TEST(ConsoleConfig, SavedValueIsRestoredOnLoad)
{
    FConsoleRegistry& Registry = FConsoleRegistry::Get();
    const float Original = Registry.GetAs<float>("DebugDraw.MaxDistance");
    const FString Path = Filesystem::GetTempDirectory() + "/LuminaConsoleConfigTest.json";
    Filesystem::RemoveFile(Path);

    ASSERT_TRUE(Registry.SetAs<float>("DebugDraw.MaxDistance", 123.5f));
    ASSERT_TRUE(Registry.SaveToFile(Path));

    ASSERT_TRUE(Registry.SetAs<float>("DebugDraw.MaxDistance", 7.0f));
    ASSERT_TRUE(Registry.LoadFromFile(Path));
    EXPECT_EQ(Registry.GetAs<float>("DebugDraw.MaxDistance"), 123.5f);

    Registry.SetAs<float>("DebugDraw.MaxDistance", Original);
    ASSERT_TRUE(Registry.SaveToFile(Path));
    FString Text;
    ASSERT_TRUE(Filesystem::ReadFile(Text, Path));
    EXPECT_EQ(Text.Find("DebugDraw.MaxDistance"), Constants::kIndexNone);

    Filesystem::RemoveFile(Path);
}

TEST(ConsoleConfig, SetFromCodeDoesNotTouchSavedFile)
{
    FConsoleRegistry& Registry = FConsoleRegistry::Get();
    const FString Path = FConsoleRegistry::GetConfigFilePath();
    const bool bExistedBefore = Filesystem::Exists(Path);
    FString Before;
    if (bExistedBefore)
    {
        ASSERT_TRUE(Filesystem::ReadFile(Before, Path));
    }
    const TOptional<FString> Original = Registry.GetValueAsString("DebugDraw.MaxDistance");
    ASSERT_TRUE(Original.IsSet());

    ASSERT_TRUE(Registry.SetValueFromString("DebugDraw.MaxDistance", "321.25"));
    ASSERT_TRUE(Registry.SetValueFromString("DebugDraw.MaxDistance", Original->View()));

    EXPECT_EQ(Filesystem::Exists(Path), bExistedBefore);
    if (bExistedBefore)
    {
        FString After;
        ASSERT_TRUE(Filesystem::ReadFile(After, Path));
        EXPECT_EQ(After, Before);
    }
}
