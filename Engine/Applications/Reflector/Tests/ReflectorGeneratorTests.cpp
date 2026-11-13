#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Platform/Process/PlatformProcess.h"

#if defined(LE_PLATFORM_WINDOWS)

namespace LuminaReflectorGeneratorTests
{
    namespace Platform = Lumina::Platform;
    namespace fs = std::filesystem;

    constexpr const char* kFixtureHeader = R"cpp(#pragma once
#include "ReflectionMacros.h"
#include "fixturetypes.generated.h"

REFLECT()
struct SFixtureThing
{
    GENERATED_BODY()

    PROPERTY(FixtureTag)
    int Tagged = 0;

    PROPERTY()
    int Untagged = 0;
};
)cpp";

    constexpr uint32_t kRunTimeoutMilliseconds = 120000;

    struct FRun
    {
        int         ExitCode = 0;
        std::string Output;

        bool Says(const std::string& Text) const { return Output.find(Text) != std::string::npos; }
    };

    // Each test drives the real Reflector over one fixture header, with ReflectorTestGenerator loaded in a given mode.
    class ReflectorGenerator : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            Binaries = fs::path(*Platform::GetCurrentProcessPath()).parent_path();
            Workspace = fs::temp_directory_path() / (std::string("LuminaReflectorTests_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(Workspace);
            fs::create_directories(Workspace / "Fixture");
            std::ofstream(Workspace / "Fixture" / "FixtureTypes.h", std::ios::binary) << kFixtureHeader;
        }

        void TearDown() override
        {
            std::error_code Ec;
            fs::remove_all(Workspace, Ec);
        }

        // An empty Library runs the built-in generators alone.
        FRun Run(const fs::path& Library, const std::string& Mode, const std::string& Arguments = {}) const
        {
            const fs::path ReflectionMacros = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "Source" / "Runtime" / "Source" / "Core" / "Reflection";

            std::ostringstream Input;
            Input << "LuminaReflector 1\nWorkspace Fixture\nWorkspacePath " << Workspace.generic_string() << "\n";
            if (!Library.empty())
            {
                Input << "Generator " << Library.generic_string() << "\nGeneratorRules " << RulesFile().generic_string() << "\nGeneratorOption Mode=" << Mode << "\n";
            }
            Input << "Module Fixture\nPath " << (Workspace / "Fixture").generic_string()
                  << "\nGeneratedDir " << GeneratedDir().generic_string()
                  << "\nInclude " << ReflectionMacros.generic_string()
                  << "\nHeader " << (Workspace / "Fixture" / "FixtureTypes.h").generic_string() << "\nEnd\n";
            std::ofstream(Workspace / "Input.txt", std::ios::binary) << Input.str();

            const std::wstring Parameters = L"\"" + (Workspace / "Input.txt").wstring() + L"\" -strict-parse " + fs::path(Arguments).wstring();
            FRun Result;
            Result.ExitCode = Platform::RunProcessAndWaitCapture((Binaries / "Reflector.exe").c_str(), Parameters.c_str(), nullptr,
                [&Result](Lumina::FStringView Line) { Result.Output.append(Line.GetData(), Line.Len()).push_back('\n'); }, kRunTimeoutMilliseconds);
            return Result;
        }

        fs::path FixtureLibrary() const { return Binaries / (LUMINA_SHAREDLIB_PREFIX_NAME "ReflectorTestGenerator" LUMINA_SHAREDLIB_EXT_NAME); }
        fs::path RulesFile() const { return Workspace / "ReflectorTestGenerator.Build.cs"; }
        fs::path GeneratedDir() const { return Workspace / "Generated"; }
        fs::path FixtureReport() const { return GeneratedDir() / "Fixture.fixture.txt"; }
        fs::path BuiltinShard() const { return GeneratedDir() / "ReflectionUnity_0.gen.cpp"; }

        static std::string ReadFile(const fs::path& Path)
        {
            std::ifstream Stream(Path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>());
        }

        fs::path Binaries;
        fs::path Workspace;
    };

    TEST_F(ReflectorGenerator, AnUnclaimedSpecifierFailsTheRun)
    {
        const FRun Result = Run({}, {});

        EXPECT_NE(Result.ExitCode, 0);
        EXPECT_TRUE(Result.Says("error LRT1009: PROPERTY specifier 'FixtureTag' is not recognized")) << Result.Output;
    }

    TEST_F(ReflectorGenerator, ALoadedGeneratorClaimsItsSpecifierAndRunsOverTheModel)
    {
        const FRun Result = Run(FixtureLibrary(), "Report");

        ASSERT_EQ(Result.ExitCode, 0) << Result.Output;
        EXPECT_FALSE(Result.Says("LRT1009")) << Result.Output;
        EXPECT_EQ(ReadFile(FixtureReport()), "SFixtureThing.Tagged\n");
        EXPECT_FALSE(ReadFile(BuiltinShard()).empty());
    }

    TEST_F(ReflectorGenerator, ALibraryWithoutTheEntryPointsIsRefused)
    {
        const FRun Result = Run(Binaries / "libclang.dll", "Report");

        EXPECT_NE(Result.ExitCode, 0);
        EXPECT_TRUE(Result.Says(RulesFile().generic_string() + "(1,0): error LRT9006")) << Result.Output;
        EXPECT_TRUE(Result.Says("exports no LUMINA_REFLECTION_GENERATOR entry points")) << Result.Output;
    }

    class ReflectorGeneratorFailure : public ReflectorGenerator, public ::testing::WithParamInterface<const char*>
    {
    };

    TEST_P(ReflectorGeneratorFailure, LeavesTheBuiltinOutputsWholeAndReportsAtTheRulesFile)
    {
        const FRun Failed = Run(FixtureLibrary(), GetParam(), "-generator-timeout=1");

        EXPECT_NE(Failed.ExitCode, 0);
        EXPECT_TRUE(Failed.Says(RulesFile().generic_string() + "(1,0): error LRT9006")) << Failed.Output;
        EXPECT_FALSE(fs::exists(FixtureReport()));
        const std::string ShardAfterFailure = ReadFile(BuiltinShard());

        ASSERT_EQ(Run(FixtureLibrary(), "Report").ExitCode, 0);
        EXPECT_EQ(ShardAfterFailure, ReadFile(BuiltinShard()));
    }

    INSTANTIATE_TEST_SUITE_P(Modes, ReflectorGeneratorFailure, ::testing::Values("Throw", "Error", "Crash", "Hang", "Clobber"));
}

#endif
