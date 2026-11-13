#include <gtest/gtest.h>

#include "Cooker/ProjectPackager.h"

using namespace Lumina;

TEST(ProjectPackagerPlatform, ExecutableNaming)
{
    const FPackagePlatformRules Windows = FPackagePlatformRules::For(EPackagePlatform::Windows64);
    const FPackagePlatformRules Linux = FPackagePlatformRules::For(EPackagePlatform::Linux64);

    EXPECT_EQ(Windows.ExecutableFileName("MyGame"), FString("MyGame.exe"));
    EXPECT_EQ(Linux.ExecutableFileName("MyGame"), FString("MyGame"));

    EXPECT_TRUE(Windows.IsExecutable("Lumina-Shipping.exe"));
    EXPECT_FALSE(Windows.IsExecutable("Lumina-Shipping"));
    EXPECT_TRUE(Linux.IsExecutable("Lumina-Shipping"));
    EXPECT_FALSE(Linux.IsExecutable("Lumina-Shipping.exe"));
    EXPECT_FALSE(Linux.IsExecutable("libRuntime-Shipping.so"));
}

TEST(ProjectPackagerPlatform, LibraryExtensions)
{
    const FPackagePlatformRules Windows = FPackagePlatformRules::For(EPackagePlatform::Windows64);
    const FPackagePlatformRules Linux = FPackagePlatformRules::For(EPackagePlatform::Linux64);

    EXPECT_TRUE(Windows.IsLibrary("Runtime-Shipping.dll"));
    EXPECT_TRUE(Windows.IsLibrary("SDL3.DLL"));
    EXPECT_FALSE(Windows.IsLibrary("libRuntime-Shipping.so"));
    EXPECT_TRUE(Linux.IsLibrary("libRuntime-Shipping.so"));
    EXPECT_TRUE(Linux.IsLibrary("libslang-compiler.so.0.2026.3.1"));
    EXPECT_FALSE(Linux.IsLibrary("Runtime-Shipping.dll"));
    EXPECT_FALSE(Windows.IsLibrary("libslang-compiler.so.0.2026.3.1"));

    EXPECT_EQ(Windows.LibraryFileName("Runtime-Shipping"), FString("Runtime-Shipping.dll"));
    EXPECT_EQ(Linux.LibraryFileName("Runtime-Shipping"), FString("libRuntime-Shipping.so"));

    EXPECT_EQ(Windows.ModuleStem("Runtime-Shipping.dll"), FStringView("Runtime-Shipping"));
    EXPECT_EQ(Windows.ModuleStem("libclang.dll"), FStringView("libclang"));
    EXPECT_EQ(Linux.ModuleStem("libRuntime-Shipping.so"), FStringView("Runtime-Shipping"));
    EXPECT_EQ(Linux.ModuleStem("Lumina-Shipping"), FStringView("Lumina-Shipping"));
}

TEST(ProjectPackagerPlatform, BinariesFolder)
{
    EXPECT_EQ(FPackagePlatformRules::For(EPackagePlatform::Windows64).BinariesDirectory("C:/Engine"), FFixedString("C:/Engine/Binaries/Windows64"));
    EXPECT_EQ(FPackagePlatformRules::For(EPackagePlatform::Linux64).BinariesDirectory("/opt/Engine"), FFixedString("/opt/Engine/Binaries/Linux64"));
}

TEST(ProjectPackagerPlatform, DotNetRuntimeAndThirdPartyPlacement)
{
    const FPackagePlatformRules Windows = FPackagePlatformRules::For(EPackagePlatform::Windows64);
    const FPackagePlatformRules Linux = FPackagePlatformRules::For(EPackagePlatform::Linux64);

    EXPECT_EQ(Windows.DotNetRuntimeRid, FStringView("win-x64"));
    EXPECT_EQ(Linux.DotNetRuntimeRid, FStringView("linux-x64"));

    // Third-party libraries leave the executable's folder only where delay-loading lets the exe start without them.
    EXPECT_TRUE(Windows.bThirdPartyToDataPlugins);
    EXPECT_FALSE(Linux.bThirdPartyToDataPlugins);
}

TEST(ProjectPackagerPlatform, SkipRules)
{
    const FPackagePlatformRules Windows = FPackagePlatformRules::For(EPackagePlatform::Windows64);
    const FPackagePlatformRules Linux = FPackagePlatformRules::For(EPackagePlatform::Linux64);

    for (const FPackagePlatformRules& Rules : { Windows, Linux })
    {
        const auto Lib = [&Rules](const char* Stem) { return Rules.LibraryFileName(Stem); };

        EXPECT_FALSE(Rules.IsSkippedBinary(*Lib("Runtime-Shipping"), "Shipping"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Lib("Runtime-Development"), "Shipping"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Lib("Runtime-Editor-Shipping"), "Shipping"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Lib("Editor-Shipping"), "Shipping"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Lib("Runtime-Shipping"), "Server-Shipping"));
        EXPECT_FALSE(Rules.IsSkippedBinary(*Lib("Runtime-Server-Shipping"), "Server-Shipping"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Lib("GFSDK_Aftermath_Lib.x64"), "Shipping"));
        EXPECT_FALSE(Rules.IsSkippedBinary(*Lib("GFSDK_Aftermath_Lib.x64"), "Development"));
        EXPECT_TRUE(Rules.IsSkippedBinary(*Rules.ExecutableFileName("Lumina-Development"), "Shipping"));
        EXPECT_FALSE(Rules.IsSkippedBinary(*Rules.ExecutableFileName("Lumina-Shipping"), "Shipping"));
    }

    EXPECT_TRUE(Windows.IsSkippedBinary("libclang.dll", "Development"));
    EXPECT_TRUE(Windows.IsSkippedBinary("nvperf_grfx_host.dll", "Development"));
    EXPECT_TRUE(Windows.IsSkippedBinary("BugSplat64.dll", "Development"));
    EXPECT_FALSE(Windows.IsSkippedBinary("libclang.pdb", "Development"));
    EXPECT_TRUE(Linux.IsSkippedBinary("libclang.so", "Development"));
    EXPECT_TRUE(Linux.IsSkippedBinary("libnvperf_grfx_host.so", "Development"));
    EXPECT_FALSE(Linux.IsSkippedBinary("libslang.so", "Development"));
}
