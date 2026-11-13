#include <gtest/gtest.h>
#include "Containers/Span.h"
#include "Core/Windows/Window.h"
#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "LogCountTestHelpers.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

using namespace Lumina;

TEST(WindowIconTests, EngineIconDecodesToRGBA)
{
    Paths::InitializePaths();
    Windowing::FWindowIcon Icon;
    ASSERT_TRUE(Windowing::LoadWindowIcon(Paths::GetEngineResourceDirectory() + "/Textures/Lumina.png", Icon));
    EXPECT_GT(Icon.Width, 0);
    EXPECT_GT(Icon.Height, 0);
    EXPECT_EQ(Icon.Pixels.Num(), Icon.Width * Icon.Height * 4);
}

TEST(WindowIconTests, EmptyPathLoadsNothingAndStaysQuiet)
{
    const int32 WarningsBefore = CountLoggedMessages(ELogLevel::Warn, "Failed to load window icon");
    Windowing::FWindowIcon Icon;
    EXPECT_FALSE(Windowing::LoadWindowIcon("", Icon));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "Failed to load window icon"), WarningsBefore);
}

TEST(WindowIconTests, MissingFileWarnsOnceWithPath)
{
    const FString Path = Format("{}/MissingIcon_{}.png", Filesystem::GetTempDirectory(), FGuid::New());
    Windowing::FWindowIcon Icon;
    EXPECT_FALSE(Windowing::LoadWindowIcon(Path, Icon));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "Failed to load window icon", Path), 1);
}

TEST(WindowIconTests, UndecodableFileWarnsOnceWithPath)
{
    const FString Path = Format("{}/BadIcon_{}.png", Filesystem::GetTempDirectory(), FGuid::New());
    const char NotAPng[] = "not a png";
    ASSERT_TRUE(Filesystem::WriteFile(Path, TSpan<const uint8>(reinterpret_cast<const uint8*>(NotAPng), sizeof(NotAPng))));

    Windowing::FWindowIcon Icon;
    EXPECT_FALSE(Windowing::LoadWindowIcon(Path, Icon));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "Failed to load window icon", Path), 1);

    Filesystem::RemoveFile(Path);
}
