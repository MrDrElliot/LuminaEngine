#include <gtest/gtest.h>
#include "GUID/GUID.h"

using namespace Lumina;

// Same seed must produce the same GUID. Regression for the hash +
// shift-past-type-width UB that made Debug and Development emit different bytes.
TEST(GUIDTests, NewDeterministic_SameSeedSameGuid)
{
    FGuid A = FGuid::NewDeterministic("Engine.PrimitiveMesh.Cube");
    FGuid B = FGuid::NewDeterministic("Engine.PrimitiveMesh.Cube");

    EXPECT_EQ(A, B);
    EXPECT_EQ(A.GetBytes(), B.GetBytes());
}

TEST(GUIDTests, NewDeterministic_DifferentSeedsDifferentGuids)
{
    FGuid Cube     = FGuid::NewDeterministic("Engine.PrimitiveMesh.Cube");
    FGuid Sphere   = FGuid::NewDeterministic("Engine.PrimitiveMesh.Sphere");
    FGuid Capsule  = FGuid::NewDeterministic("Engine.PrimitiveMesh.Capsule");

    EXPECT_NE(Cube, Sphere);
    EXPECT_NE(Cube, Capsule);
    EXPECT_NE(Sphere, Capsule);
}

// One-character difference must change the result. Catches a bug where the
// hash collapsed long suffixes (e.g. only the first N bytes mattered).
TEST(GUIDTests, NewDeterministic_SmallSeedChangeChangesGuid)
{
    FGuid A = FGuid::NewDeterministic("Mesh.A");
    FGuid B = FGuid::NewDeterministic("Mesh.B");

    EXPECT_NE(A, B);
}

TEST(GUIDTests, NewDeterministic_EmptySeedIsValid)
{
    FGuid G = FGuid::NewDeterministic("");

    EXPECT_TRUE(G.IsValid());
    EXPECT_NE(G, FGuid::Empty());
}

// Per the implementation: high nibble of byte[6] is the version (5,
// name-based), and the top two bits of byte[8] are the RFC 4122 variant.
TEST(GUIDTests, NewDeterministic_VersionAndVariantBits)
{
    FGuid G = FGuid::NewDeterministic("Engine.PrimitiveMesh.Cube");
    const auto& B = G.GetBytes();

    EXPECT_EQ((B[6] & 0xF0), 0x50);
    EXPECT_EQ((B[8] & 0xC0), 0x80);
}

// High 8 bytes derive from the low 8 via SplitMix64; the halves must differ
// (a copy was the symptom of the old shift-past-type-width UB).
TEST(GUIDTests, NewDeterministic_HighAndLowHalvesDiffer)
{
    FGuid G = FGuid::NewDeterministic("Engine.PrimitiveMesh.Cube");
    const auto& B = G.GetBytes();

    bool AnyDifferent = false;
    for (size_t i = 0; i < 8; ++i)
    {
        if (B[i] != B[i + 8])
        {
            AnyDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(AnyDifferent);
}

TEST(GUIDTests, ToString_ProducesNonEmpty)
{
    FGuid G = FGuid::NewDeterministic("Engine.PrimitiveMesh.Sphere");
    FString Text = G.ToString();
    EXPECT_FALSE(Text.empty());
    EXPECT_EQ(Text.length(), 36u);
}

TEST(GUIDTests, ToShortString_ProducesNonEmpty)
{
    FGuid G = FGuid::NewDeterministic("Engine.PrimitiveMesh.Sphere");
    FString Text = G.ToShortString();
    EXPECT_FALSE(Text.empty());
    EXPECT_EQ(Text.length(), 32u);
}

TEST(GUIDTests, NewDeterministic_RoundTripThroughString)
{
    FGuid Original  = FGuid::NewDeterministic("Engine.PrimitiveMesh.Sphere");
    FString  AsText = Original.ToString();
    FGuid Restored  = FGuid::FromString(AsText);

    EXPECT_EQ(Original, Restored);
}

// Determinism is per-seed-bits, not per-FStringView identity: same characters
// through different FStringView constructors must yield identical results.
TEST(GUIDTests, NewDeterministic_StringViewSourceIndependence)
{
    const char* Literal = "Engine.PrimitiveMesh.Plane";
    FString     Owned   = "Engine.PrimitiveMesh.Plane";

    FGuid FromLiteral = FGuid::NewDeterministic(FStringView(Literal));
    FGuid FromOwned   = FGuid::NewDeterministic(FStringView(Owned.c_str(), Owned.length()));

    EXPECT_EQ(FromLiteral, FromOwned);
}

TEST(GUIDTests, Empty_IsNotValid)
{
    EXPECT_FALSE(FGuid::Empty().IsValid());
    EXPECT_FALSE(FGuid{}.IsValid());
    EXPECT_EQ(FGuid::Empty(), FGuid{});
    EXPECT_EQ(FGuid::Empty(), FGuid::Invalid());
}

TEST(GUIDTests, Invalidate_ProducesEmpty)
{
    FGuid G = FGuid::NewDeterministic("anything");
    ASSERT_TRUE(G.IsValid());

    G.Invalidate();
    EXPECT_FALSE(G.IsValid());
    EXPECT_EQ(G, FGuid::Empty());
}

TEST(GUIDTests, New_ProducesUniqueValidGuids)
{
    FGuid A = FGuid::New();
    FGuid B = FGuid::New();

    EXPECT_TRUE(A.IsValid());
    EXPECT_TRUE(B.IsValid());
    EXPECT_NE(A, B);
}

TEST(GUIDTests, Hash_ConsistentWithEquality)
{
    FGuid A = FGuid::NewDeterministic("seed-x");
    FGuid B = FGuid::NewDeterministic("seed-x");

    EXPECT_EQ(A, B);
    EXPECT_EQ(A.Hash(), B.Hash());
}

TEST(GUIDTests, FromString_RoundTripWithDashes)
{
    FGuid Original = FGuid::NewDeterministic("round-trip-dashes");
    FGuid Restored = FGuid::FromString(Original.ToString(true, true));

    EXPECT_EQ(Original, Restored);
}

TEST(GUIDTests, FromString_RoundTripWithoutDashes)
{
    FGuid Original = FGuid::NewDeterministic("round-trip-no-dashes");
    FGuid Restored = FGuid::FromString(Original.ToShortString());

    EXPECT_EQ(Original, Restored);
}

TEST(GUIDTests, TryParse_RejectsGarbage)
{
    EXPECT_FALSE(FGuid::TryParse("not a guid").has_value());
    EXPECT_FALSE(FGuid::TryParse("").has_value());
    EXPECT_FALSE(FGuid::TryParse("ZZZZZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZZZZZZZZZ").has_value());
}

TEST(GUIDTests, TryParse_AcceptsValid)
{
    FGuid Original = FGuid::NewDeterministic("valid-parse");
    auto  Parsed   = FGuid::TryParse(Original.ToString());

    ASSERT_TRUE(Parsed.has_value());
    EXPECT_EQ(*Parsed, Original);
}

TEST(GUIDTests, ComparisonOperators)
{
    FGuid Lower  = FGuid::FromString("00000000-0000-0000-0000-000000000001");
    FGuid Higher = FGuid::FromString("00000000-0000-0000-0000-000000000002");

    EXPECT_LT(Lower, Higher);
    EXPECT_LE(Lower, Higher);
    EXPECT_LE(Lower, Lower);
    EXPECT_GT(Higher, Lower);
    EXPECT_GE(Higher, Lower);
    EXPECT_GE(Higher, Higher);
}
