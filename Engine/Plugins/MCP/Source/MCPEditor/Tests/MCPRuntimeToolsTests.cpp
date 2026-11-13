#include "MCPRuntimeTools.h"
#include "MCPToolTestUtil.h"
#include "ScopedTempMountTestUtil.h"

#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPRuntimeToolsTestsInternal
{
    class MCPRuntimeToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPRuntimeToolsTest() : FRegisteredToolsTest("MCPRuntimeToolsTests", &RegisterRuntimeTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();

            PreviousConfig = Global::GConfig;
            Global::GConfig = &Config;
            Config.DiscoverAndLoadSettings();
        }

        void TearDown() override
        {
            Global::GConfig = PreviousConfig;
            FRegisteredToolsTest::TearDown();
        }

        FJsonValue SetTickRate(int32 Value)
        {
            return ExpectSucceeded("engine.settings", FStringView(Format(
                R"({{"Action":"Set","Class":"ProjectSettings","Path":"HeadlessTickRate","Value":"{}"}})", Value)))["structuredContent"];
        }

        int32 SavedTickRate()
        {
            FString Text;
            FJsonValue Saved;
            EXPECT_TRUE(VFS::ReadFile(Text, FString("/Config/GameSettings.json")));
            EXPECT_TRUE(FJsonValue::Parse(Text.View(), Saved)) << *Text;
            return Saved["ProjectSettings"]["HeadlessTickRate"].AsInt32(-1);
        }

        static CProjectSettings* ProjectSettings() { return CProjectSettings::StaticClass()->GetDefaultObject<CProjectSettings>(); }

        FConfig Config;
        FConfig* PreviousConfig = nullptr;
        TestUtil::FScopedTempMount Mount{ "/Config", "LuminaMCPRuntimeToolsTests" };
    };
}

using namespace MCPRuntimeToolsTestsInternal;

TEST_F(MCPRuntimeToolsTest, StatsReportsFrameWindowMemoryAndWorlds)
{
    const FJsonValue Stats = ExpectSucceeded("engine.stats", R"({"Worst":3})")["structuredContent"];
    for (const char* Field : { "Frames", "WindowSeconds", "MeanMs", "P99Ms", "MaxMs", "Over50Ms", "GpuUsageMB", "TrackedNativeMB" })
    {
        EXPECT_TRUE(Stats[Field].IsNumber()) << Field;
    }
    EXPECT_GE(Stats["Frames"].AsInt32(-1), 0);
    EXPECT_GT(Stats["ProcessMemoryMB"].AsInt32(), 0);
    EXPECT_GT(Stats["LiveObjects"].AsInt32(), 0);
    EXPECT_TRUE(Stats["Worst"].IsArray());
    EXPECT_LE(Stats["Worst"].Num(), 3);
    EXPECT_TRUE(Stats["Worlds"].IsArray());
}

TEST_F(MCPRuntimeToolsTest, MemoryReportsProcessLedgerAndCategories)
{
    const FJsonValue Report = ExpectSucceeded("engine.memory", R"({"Top":4,"bCollectManaged":false})")["structuredContent"];
    EXPECT_GT(Report["ProcessMB"].AsInt32(), 0);
    EXPECT_GT(Report["PrivateCommittedMB"].AsFloat(), 0.0f);
    EXPECT_GE(Report["TrackedNativeMB"].AsFloat(-1.0f), 0.0f);
    EXPECT_GE(Report["TrackedAllocations"].AsInt32(-1), 0);
    EXPECT_TRUE(Report["bCallstacks"].IsBool());
    EXPECT_TRUE(Report["Categories"].IsArray());
    EXPECT_LE(Report["Categories"].Num(), 4);
    EXPECT_TRUE(Report["GpuHeaps"].IsArray());
}

TEST_F(MCPRuntimeToolsTest, SettingsGetListsClassesAndReadsOneByDisplayName)
{
    const FJsonValue Listed = ExpectSucceeded("engine.settings", R"({})")["structuredContent"]["Classes"];
    bool bListed = false;
    for (int32 i = 0; i < Listed.Num(); ++i)
    {
        bListed |= FString(Listed[i]["Name"].AsString()) == "CProjectSettings";
        EXPECT_TRUE(Listed[i]["Values"].AsString().IsEmpty());
    }
    EXPECT_TRUE(bListed);

    const FJsonValue One = ExpectSucceeded("engine.settings", R"({"Class":"Project"})")["structuredContent"]["Classes"];
    ASSERT_EQ(One.Num(), 1);
    EXPECT_EQ(FString(One[0]["Name"].AsString()), "CProjectSettings");
    EXPECT_EQ(FString(One[0]["ConfigFile"].AsString()), "/Config/GameSettings.json");

    FJsonValue Values;
    ASSERT_TRUE(FJsonValue::Parse(One[0]["Values"].AsString(), Values));
    EXPECT_EQ(Values["HeadlessTickRate"].AsInt32(-1), ProjectSettings()->HeadlessTickRate);
}

TEST_F(MCPRuntimeToolsTest, SettingsSetAppliesSavesAndRestores)
{
    const int32 Original = ProjectSettings()->HeadlessTickRate;
    const int32 Changed = Original == 123 ? 124 : 123;

    const FJsonValue Set = SetTickRate(Changed);
    EXPECT_EQ(FString(Set["Previous"].AsString()), Format("{}", Original));
    EXPECT_EQ(FString(Set["Current"].AsString()), Format("{}", Changed));
    EXPECT_EQ(ProjectSettings()->HeadlessTickRate, Changed);
    EXPECT_EQ(SavedTickRate(), Changed);

    const FJsonValue Restored = SetTickRate(Original);
    EXPECT_EQ(FString(Restored["Previous"].AsString()), Format("{}", Changed));
    EXPECT_EQ(ProjectSettings()->HeadlessTickRate, Original);
    EXPECT_EQ(SavedTickRate(), Original);
}

TEST_F(MCPRuntimeToolsTest, SettingsRefuseUnknownClassPathAndValue)
{
    const int32 Original = ProjectSettings()->HeadlessTickRate;

    ExpectRefused("engine.settings", R"({"Class":"NoSuchSettings"})", "No settings class named 'NoSuchSettings'");
    ExpectRefused("engine.settings", R"({"Action":"Set","Class":"NoSuchSettings","Path":"HeadlessTickRate","Value":"1"})",
        "No settings class named 'NoSuchSettings'");
    ExpectRefused("engine.settings", R"({"Action":"Set","Class":"ProjectSettings","Path":"NoSuchField","Value":"1"})",
        "'NoSuchField' is not a field of");
    ExpectRefused("engine.settings", R"({"Action":"Set","Class":"ProjectSettings","Path":"HeadlessTickRate","Value":"not json"})",
        "Value is not JSON");

    EXPECT_EQ(ProjectSettings()->HeadlessTickRate, Original);
    EXPECT_FALSE(VFS::Exists("/Config/GameSettings.json"));
}
