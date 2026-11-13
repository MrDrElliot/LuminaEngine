#include <gtest/gtest.h>

#include "Cooker/AssetCooker.h"

using namespace Lumina;

TEST(AssetCookerDevelopmentOnlyFile, ProjectAndSolutionFiles)
{
    EXPECT_TRUE(IsDevelopmentOnlyGameFile("/Game/MyGame.sln", false));
    EXPECT_TRUE(IsDevelopmentOnlyGameFile("/Game/MyGame.slnx", false));
    EXPECT_TRUE(IsDevelopmentOnlyGameFile("/Game/MyGame.csproj", false));
    EXPECT_FALSE(IsDevelopmentOnlyGameFile("/Game/Content/Level.lasset", false));
}
