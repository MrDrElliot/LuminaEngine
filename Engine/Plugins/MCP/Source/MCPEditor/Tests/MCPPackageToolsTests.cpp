#include <gtest/gtest.h>

#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "MCPPackageTools.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace
{
    TEST(MCPPackageToolsTests, AppendSplitsErrorsFromWarnings)
    {
        FPackageSession Session;
        Session.Append("1>main.cpp(10): error C2039: not a member");
        Session.Append("1>main.cpp(20): warning C4100: unreferenced parameter");
        Session.Append("Retry 1/3 failed, retrying");
        Session.Append("note: nothing wrong here");

        EXPECT_EQ(Session.Errors, 2);
        EXPECT_EQ(Session.Warnings, 1);
        EXPECT_EQ(Session.Lines.Num(), 4);
    }

    TEST(MCPPackageToolsTests, AppendWarnTaggedFailedLineCountsAsWarningOnly)
    {
        FPackageSession Session;
        Session.Append("  [warn] failed to copy X");

        EXPECT_EQ(Session.Errors, 0);
        EXPECT_EQ(Session.Warnings, 1);
    }

    class MCPPackageToolsLogTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Unique per test so parallel test runs never share or delete each other's folder.
            Root = Format("{}/LuminaMCPPackageToolsTests_{}", Lumina::Filesystem::GetTempDirectory(), FGuid::New().ToString());
            ASSERT_TRUE(Lumina::Filesystem::MakeDirectoryTree(Root));
            Path = Root + "/Lumina.log";
        }

        void TearDown() override
        {
            Lumina::Filesystem::RemoveTree(Root);
        }

        static TSpan<const uint8> Bytes(FStringView Text)
        {
            return TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len());
        }

        static FString RepeatedLine(char Ch, int32 Count)
        {
            FString Line;
            Line.AppendChars(Ch, Count);
            Line.AppendChar('\n');
            return Line;
        }

        FString Root;
        FString Path;
    };

    TEST_F(MCPPackageToolsLogTest, ScanNewLogLinesKeepsEarlyErrorAfterItScrollsOutOfTheTail)
    {
        using namespace Lumina::Filesystem;

        ASSERT_TRUE(WriteFile(Path, Bytes("[00:00:00] [error] boom on startup\n")));

        FPackageGameSession Game;
        Game.LogPath = Path;
        Game.ScanNewLogLines();
        EXPECT_EQ(Game.Errors, 1);

        const FString FillerLine = RepeatedLine('x', 2048);
        for (int32 i = 0; i < 600; ++i)
        {
            ASSERT_TRUE(AppendFile(Path, Bytes(FillerLine)));
            Game.ScanNewLogLines();
            EXPECT_GE(Game.Errors, 1);
        }

        EXPECT_GT(FileSize(FStringView(*Path, Path.Len())), (uint64)(1024 * 1024));
        EXPECT_EQ(Game.Errors, 1);
    }

    TEST_F(MCPPackageToolsLogTest, ScanNewLogLinesIgnoresAStalePreLaunchLogThenPicksUpTheReplacement)
    {
        using namespace Lumina::Filesystem;

        // A larger leftover log from the previous run, still sitting at the path the next run will log to.
        ASSERT_TRUE(WriteFile(Path, Bytes("[00:00:00] [error] stale error from the previous run\n")));
        const FString OldFiller = RepeatedLine('y', 4096);
        for (int32 i = 0; i < 4; ++i)
        {
            ASSERT_TRUE(AppendFile(Path, Bytes(OldFiller)));
        }

        // Launch captures this baseline before the game has had a chance to rotate the leftover log away.
        const FFileStat PreLaunchStat = Stat(FStringView(*Path, Path.Len()));
        FPackageGameSession Game;
        Game.LogPath = Path;
        Game.PreLaunchLogSize = (int64)PreLaunchStat.Size;
        Game.PreLaunchLogModifyTime = PreLaunchStat.LastModifyTime;

        // A poll can land before the game has rotated the old log away; it must not count the leftover.
        Game.ScanNewLogLines();
        EXPECT_EQ(Game.Errors, 0);

        ASSERT_TRUE(WriteFile(Path, Bytes("[00:00:01] [error] boom on startup\n")));
        Game.ScanNewLogLines();
        EXPECT_EQ(Game.Errors, 1);
    }

    TEST_F(MCPPackageToolsLogTest, ScanNewLogLinesKeepsTotalsAcrossMidRunRotation)
    {
        using namespace Lumina::Filesystem;

        FPackageGameSession Game;
        Game.LogPath = Path;

        ASSERT_TRUE(WriteFile(Path, Bytes("[00:00:00] [error] first run error, padded so its cursor lands past the rotated file's size\n")));
        Game.ScanNewLogLines();
        EXPECT_EQ(Game.Errors, 1);

        // FFileSink::Rotate replaces the file at the size cap, so the new one starts smaller than the old cursor.
        ASSERT_TRUE(WriteFile(Path, Bytes("[00:00:01] [error] second run error\n")));
        Game.ScanNewLogLines();
        EXPECT_EQ(Game.Errors, 2);
    }
}
