#include <gtest/gtest.h>
#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Renderer/RenderManager.h"

using namespace Lumina;

namespace
{
    class PipelinePrewarmTest : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            Root = Format("{}/LuminaPipelinePrewarmTests_{}", Filesystem::GetTempDirectory(), FGuid::New().ToString());
            ASSERT_TRUE(Filesystem::MakeDirectoryTree(Root));
            Path = Format("{}/PipelinePrewarm.bin", Root);
        }

        void TearDown() override
        {
            Filesystem::RemoveTree(Root);
        }

        static FPipelinePrewarmRecord MakeRecord(uint64 Shader)
        {
            FPipelinePrewarmRecord Record;
            Record.Shaders[0] = Shader;
            Record.State      = 7;
            Record.Count      = 1;
            Record.Words[0]   = Shader * 3u;
            return Record;
        }

        static TVector<uint64> Offered(FScenePipelineCache& Cache, uint64 Accept)
        {
            TVector<uint64> Shaders;
            Cache.PrewarmRecorded(32, [&Shaders, Accept](const FPipelinePrewarmRecord& Record)
            {
                Shaders.Add(Record.Shaders[0]);
                return Record.Shaders[0] == Accept;
            });
            return Shaders;
        }

        FString Root;
        FString Path;
    };
}

TEST_F(PipelinePrewarmTest, ARecordedPipelineIsOfferedNextRunUntilItIsRequested)
{
    {
        FScenePipelineCache Cache;
        Cache.LoadPrewarm(Path);
        Cache.RecordPrewarm(MakeRecord(11));
        Cache.RecordPrewarm(MakeRecord(22));
        Cache.RecordPrewarm(MakeRecord(11));
        // Nothing recorded this run is offered back to it, since a miss already built it.
        EXPECT_TRUE(Offered(Cache, 0).IsEmpty());
        Cache.SavePrewarm();
    }

    FScenePipelineCache Cache;
    Cache.LoadPrewarm(Path);
    TVector<uint64> First = Offered(Cache, 11);
    ASSERT_EQ(First.Num(), 2);
    EXPECT_TRUE(First.Contains((uint64)11));
    EXPECT_TRUE(First.Contains((uint64)22));

    const TVector<uint64> Second = Offered(Cache, 0);
    ASSERT_EQ(Second.Num(), 1);
    EXPECT_EQ(Second[0], (uint64)22);
}

TEST_F(PipelinePrewarmTest, ARecordWhoseShadersNeverLoadIsDroppedAfterEightRuns)
{
    {
        FScenePipelineCache Cache;
        Cache.LoadPrewarm(Path);
        Cache.RecordPrewarm(MakeRecord(33));
        Cache.SavePrewarm();
    }

    for (int32 Run = 1; Run <= 9; ++Run)
    {
        FScenePipelineCache Cache;
        Cache.LoadPrewarm(Path);
        const TVector<uint64> Shaders = Offered(Cache, 0);
        EXPECT_EQ(Shaders.Num(), Run <= 8 ? 1 : 0) << "run " << Run;
        Cache.SavePrewarm();
    }
}

TEST_F(PipelinePrewarmTest, AnEmptyPathRecordsNothing)
{
    FScenePipelineCache Cache;
    Cache.LoadPrewarm(FString());
    Cache.RecordPrewarm(MakeRecord(44));
    Cache.SavePrewarm();
    EXPECT_TRUE(Offered(Cache, 0).IsEmpty());
    EXPECT_FALSE(Filesystem::Exists(Path));
}
