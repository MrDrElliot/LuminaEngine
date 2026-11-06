#include <gtest/gtest.h>

#include "Paths/Paths.h"

using namespace Lumina;

// The command line claims the project bare and the engine claims it with a separator, so both must match one lock.
TEST(ProjectLockPath, TrailingSeparatorsReachOneLock)
{
    EXPECT_TRUE(Paths::PathsEqual("H:/X/Proj", "H:/X/Proj/"));
    EXPECT_TRUE(Paths::PathsEqual("H:/X/Proj", "H:\\X\\Proj\\"));
    EXPECT_TRUE(Paths::PathsEqual("H:/X/Proj/", "H:\\X\\Proj\\"));
    EXPECT_FALSE(Paths::PathsEqual("H:/X/Proj", "H:/X/Proj2"));
    EXPECT_FALSE(Paths::PathsEqual("H:/X/Proj/", "H:/X/Proj2/"));
}

TEST(ProjectLockPath, TrimKeepsBareRoots)
{
    EXPECT_EQ(Paths::TrimTrailingSeparators("H:/X/Proj\\/"), FStringView("H:/X/Proj"));
    EXPECT_EQ(Paths::TrimTrailingSeparators("/"), FStringView("/"));
    EXPECT_EQ(Paths::TrimTrailingSeparators("C:/"), FStringView("C:/"));
    EXPECT_EQ(Paths::TrimTrailingSeparators("C:\\"), FStringView("C:\\"));
    EXPECT_FALSE(Paths::PathsEqual("C:/", "C:"));
}
