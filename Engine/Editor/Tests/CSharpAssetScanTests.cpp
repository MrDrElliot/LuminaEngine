#include <gtest/gtest.h>

#include "Cooker/Analyzers/CSharpAssetScan.h"

using namespace Lumina;

TEST(CSharpAssetScan, MidSegmentCutFoldsToContainingFolder)
{
    const TVector<FString> Candidates = FCSharpAssetScan::ExtractCandidates(FStringView(R"(var Path = $"/Game/Items/Sword{suffix}.lasset";)"));

    ASSERT_EQ(Candidates.Num(), 1);
    EXPECT_EQ(Candidates[0], "/Game/Items/");
}

TEST(CSharpAssetScan, BoundaryCutKeepsFolderFallback)
{
    const TVector<FString> Candidates = FCSharpAssetScan::ExtractCandidates(FStringView(R"(var Path = $"/Game/Items/{name}";)"));

    ASSERT_EQ(Candidates.Num(), 1);
    EXPECT_EQ(Candidates[0], "/Game/Items/");
}
