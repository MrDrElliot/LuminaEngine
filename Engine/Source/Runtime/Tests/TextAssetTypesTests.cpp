#include <gtest/gtest.h>

#include "Assets/AssetRegistry/TextAssetTypes.h"

using namespace Lumina;

TEST(TextAssetTypes, EmptyMetaAcceptsEveryKind)
{
    const TVector<ETextAssetKind> Kinds = TextAsset::ParseAssetTypeMeta("");
    EXPECT_EQ(Kinds.Num(), 2);
    EXPECT_EQ(TextAsset::ParseAssetTypeMeta(" ,\t").Num(), 2);
}

TEST(TextAssetTypes, RecognizedTokensParseToTheirKinds)
{
    const TVector<ETextAssetKind> Kinds = TextAsset::ParseAssetTypeMeta("rml,rcss");
    EXPECT_EQ(Kinds.Num(), 2);
}

TEST(TextAssetTypes, AnyAndWildcardAcceptEveryKind)
{
    EXPECT_EQ(TextAsset::ParseAssetTypeMeta("any").Num(), 2);
    EXPECT_EQ(TextAsset::ParseAssetTypeMeta("*").Num(), 2);
}

TEST(TextAssetTypes, UnrecognizedTokenReturnsEmptyNotEverything)
{
    const TVector<ETextAssetKind> Empty = TextAsset::ParseAssetTypeMeta("");
    const TVector<ETextAssetKind> Unrecognized = TextAsset::ParseAssetTypeMeta("rml2");

    EXPECT_TRUE(Unrecognized.IsEmpty());
    EXPECT_NE(Unrecognized.Num(), Empty.Num());
}
