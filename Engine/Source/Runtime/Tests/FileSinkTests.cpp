#include <gtest/gtest.h>
#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "Log/Sinks/FileSink.h"

using namespace Lumina;

namespace
{
    class FileSinkTest : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            Root = Format("{}/LuminaFileSinkTests_{}", Filesystem::GetTempDirectory(), FGuid::New().ToString());
            ASSERT_TRUE(Filesystem::MakeDirectoryTree(Root));
        }

        void TearDown() override
        {
            Filesystem::RemoveTree(Root);
        }

        FString Under(FStringView Relative) const
        {
            return Format("{}/{}", Root, Relative);
        }

        static void WriteLine(FFileSink& Sink, FStringView Message)
        {
            const Logging::FLogTimestamp Stamp = { "2026-11-06", "12:00:00", 0 };
            Sink.Write({ Message, &Stamp, ELogLevel::Info, 1 });
            Sink.Flush();
        }

        static FString ReadText(FStringView Path)
        {
            FString Text;
            Filesystem::ReadFile(Text, Path);
            return Text;
        }

        static int32 FindIn(const FString& Text, FStringView Needle)
        {
            return FStringView(Text.GetData(), Text.Len()).Find(Needle);
        }

        FString Root;
    };
}

TEST_F(FileSinkTest, RotationKeepsAtMostMaxFiles)
{
    const FString Live = Under("Logs/Game.log");
    ASSERT_TRUE(Filesystem::MakeParentDirectoryTree(Live));
    ASSERT_TRUE(Filesystem::WriteFile(Live, TSpan64<const uint8>(reinterpret_cast<const uint8*>("previous"), 8)));

    {
        FFileSink Sink(Live, 1, 2);
        ASSERT_TRUE(Sink.IsOpen());
        EXPECT_NE(FindIn(ReadText(Under("Logs/Game.1.log")), "previous"), Constants::kIndexNone);

        WriteLine(Sink, "first");
        WriteLine(Sink, "second");
        WriteLine(Sink, "third");
    }

    EXPECT_TRUE(Filesystem::IsFile(Live));
    EXPECT_NE(FindIn(ReadText(Under("Logs/Game.1.log")), "third"), Constants::kIndexNone);
    EXPECT_NE(FindIn(ReadText(Under("Logs/Game.2.log")), "second"), Constants::kIndexNone);
    EXPECT_FALSE(Filesystem::Exists(Under("Logs/Game.3.log")));
}

TEST_F(FileSinkTest, RetargetMovesBootLinesAndAppends)
{
    const FString Boot = Under("Engine/Boot.log");
    const FString Project = Under("Project/Logs/Game.log");
    ASSERT_TRUE(Filesystem::MakeParentDirectoryTree(Project));
    ASSERT_TRUE(Filesystem::WriteFile(Project, TSpan64<const uint8>(reinterpret_cast<const uint8*>("earlier run"), 11)));

    {
        FFileSink Sink(Boot, 1024 * 1024, 3);
        WriteLine(Sink, "boot line");

        Sink.Retarget(Project);
        EXPECT_TRUE(Sink.IsOpen());
        EXPECT_EQ(Sink.GetBasePath(), Project);

        WriteLine(Sink, "project line");
    }

    EXPECT_FALSE(Filesystem::Exists(Boot));
    EXPECT_NE(FindIn(ReadText(Under("Project/Logs/Game.1.log")), "earlier run"), Constants::kIndexNone);

    const FString Text = ReadText(Project);
    const int32 BootAt = FindIn(Text, "boot line");
    const int32 ProjectAt = FindIn(Text, "project line");
    ASSERT_NE(BootAt, Constants::kIndexNone);
    ASSERT_NE(ProjectAt, Constants::kIndexNone);
    EXPECT_LT(BootAt, ProjectAt);
}
