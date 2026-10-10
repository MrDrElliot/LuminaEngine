#include "gtest/gtest.h"

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"

// Round-trip stays inside half a grid step and decode is bit-reproducible; Common.slang must match.

using namespace Lumina;

namespace
{
    struct FRoundTrip
    {
        float MaxError = 0.0f;
        float Step     = 0.0f;
    };

    FRoundTrip RoundTrip(const TVector<FVector3>& Positions)
    {
        const FMeshPositionGrid Grid = ComputeMeshPositionGrid(Positions.data(), Positions.size());

        FRoundTrip Result;
        Result.Step = PositionGridScale(Grid.Exponent);

        for (const FVector3& P : Positions)
        {
            const FVector3 D = DecodeMeshPosition(Grid, EncodeMeshPosition(Grid, P));
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.x - P.x));
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.y - P.y));
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.z - P.z));
        }

        return Result;
    }

    // 64 vertices over a box of the given extent, centered on Origin.
    TVector<FVector3> MakePatch(FVector3 Origin, float Extent)
    {
        TVector<FVector3> Out;
        Out.reserve(MESHLET_MAX_VERTICES);

        for (uint32 i = 0; i < MESHLET_MAX_VERTICES; ++i)
        {
            // Deterministic spread with no RNG, so a failure reproduces from the test name alone.
            const float U = (float)(i % 4) / 3.0f;
            const float V = (float)((i / 4) % 4) / 3.0f;
            const float W = (float)((i / 16) % 4) / 3.0f;

            Out.push_back(FVector3(Origin.x + (U - 0.5f) * Extent,
                                   Origin.y + (V - 0.5f) * Extent,
                                   Origin.z + (W - 0.5f) * Extent));
        }
        return Out;
    }

    // A legacy meshlet with a zero anchor and exponent, so its positions decode as the raw offsets.
    FLegacyMeshlet MakeLegacyMeshlet(uint32 VertexOffset, uint32 VertexCount)
    {
        FLegacyMeshlet M{};
        M.VertexOffset = VertexOffset;
        M.VertexCount  = VertexCount;
        return M;
    }

    FLegacyMeshletVertex MakeLegacyVertex(const FVector3& Position)
    {
        FLegacyMeshletVertex V{};
        V.PositionX = (uint16)Position.x;
        V.PositionY = (uint16)Position.y;
        V.PositionZ = (uint16)Position.z;
        V.NormalZ   = 32767;
        V.Color     = 0xFFFFFFFFu;
        return V;
    }
}

TEST(MeshQuantization, RoundTripWithinHalfAStep)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(-40.0f, 3.0f, 120.0f), 75.0f));
    EXPECT_LE(R.MaxError, R.Step * 0.5f + 1e-6f);
}

// A small mesh far from the origin overflows the 24-bit anchor unless both bounds are taken.
TEST(MeshQuantization, FarFromOriginDoesNotWrapAnchor)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(1000.0f, -750.0f, 1200.0f), 0.1f));

    EXPECT_LE(R.MaxError, R.Step * 0.5f);
    EXPECT_LT(R.MaxError, 0.01f);
}

TEST(MeshQuantization, LargeMeshStillRoundTrips)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(0.0f, 0.0f, 0.0f), 4096.0f));
    EXPECT_LE(R.MaxError, R.Step * 0.5f);
}

TEST(MeshQuantization, DegenerateMeshIsExact)
{
    // Zero extent falls back to the anchor bound, and encode must not divide by zero or NaN.
    const TVector<FVector3> Positions(MESHLET_MAX_VERTICES, FVector3(3.5f, -2.25f, 0.0f));

    const FRoundTrip R = RoundTrip(Positions);
    EXPECT_LE(R.MaxError, R.Step * 0.5f);
}

TEST(MeshQuantization, EmptyMeshIsSafe)
{
    const FMeshPositionGrid Grid = ComputeMeshPositionGrid(nullptr, 0);
    EXPECT_EQ(Grid.AnchorX, 0);
    EXPECT_EQ(Grid.AnchorY, 0);
    EXPECT_EQ(Grid.AnchorZ, 0);
    EXPECT_EQ(Grid.Exponent, 0);
}

// Decode reads only the grid and the packed position, so two callers land on the same bits.
TEST(MeshQuantization, DecodeIsBitwiseReproducible)
{
    const TVector<FVector3> Positions = MakePatch(FVector3(17.0f, -4.0f, 9.5f), 3.0f);
    const FMeshPositionGrid Grid = ComputeMeshPositionGrid(Positions.data(), Positions.size());

    for (const FVector3& P : Positions)
    {
        const FMeshVertexPosition Packed = EncodeMeshPosition(Grid, P);
        const FVector3 A = DecodeMeshPosition(Grid, Packed);
        const FVector3 B = DecodeMeshPosition(Grid, Packed);

        EXPECT_EQ(memcmp(&A, &B, sizeof(FVector3)), 0);
    }
}

// Y straddles the two words, so all-ones on each axis must come back without bleeding into its neighbors.
TEST(MeshQuantization, PackingKeepsAxesApart)
{
    FMeshPositionGrid Grid;
    const float Step = PositionGridScale(Grid.Exponent);
    const float Max  = (float)MESH_POSITION_MAX * Step;

    const FVector3 Cases[] = { FVector3(Max, 0.0f, 0.0f), FVector3(0.0f, Max, 0.0f), FVector3(0.0f, 0.0f, Max), FVector3(Max, Max, Max) };
    for (const FVector3& P : Cases)
    {
        const FVector3 D = DecodeMeshPosition(Grid, EncodeMeshPosition(Grid, P));
        EXPECT_EQ(D.x, P.x);
        EXPECT_EQ(D.y, P.y);
        EXPECT_EQ(D.z, P.z);
    }
}

TEST(MeshQuantization, StructLayoutsMatchTheGPUMirrors)
{
    // Keep these in step with the static_asserts in Vertex.h and the mirrors in Common.slang.
    EXPECT_EQ(sizeof(FMeshlet), 16u);
    EXPECT_EQ(sizeof(FMeshVertexAttributes), 12u);
    EXPECT_EQ(sizeof(FMeshVertexPosition), 8u);
    EXPECT_EQ(sizeof(FMeshletSkinnedVertex), 36u);
    EXPECT_EQ(sizeof(FMeshletHeaderGPU), 176u);
}

// The shader unpacks the last word with the MESHLET_* shifts, so the bitfield order is part of the format.
TEST(MeshQuantization, MeshletPackingMatchesTheShaderShifts)
{
    FMeshlet M{};
    M.VertexCount      = 63u;
    M.TriangleCount    = 64u;
    M.LODIndex         = 5u;
    M.bShortVertexRefs = 1u;

    uint32 Words[4];
    memcpy(Words, &M, sizeof(Words));
    EXPECT_EQ(Words[3], 63u | (64u << MESHLET_TRIANGLE_COUNT_SHIFT) | (5u << MESHLET_LOD_SHIFT) | (1u << MESHLET_SHORT_REFS_SHIFT));
}

TEST(MeshQuantization, VertexRefsRoundTripShortAndLong)
{
    FMeshletData Data;

    const uint32 Near[] = { 70000u, 70005u, 70001u };
    FMeshlet ShortMeshlet{};
    AppendMeshletVertexRefs(Data, ShortMeshlet, Near, 3u);
    EXPECT_TRUE(ShortMeshlet.HasShortVertexRefs());
    EXPECT_EQ(ShortMeshlet.BaseVertex, 70000u);

    const uint32 Far[] = { 5u, 200000u };
    FMeshlet LongMeshlet{};
    AppendMeshletVertexRefs(Data, LongMeshlet, Far, 2u);
    EXPECT_FALSE(LongMeshlet.HasShortVertexRefs());

    for (uint32 i = 0; i < 3u; ++i)
    {
        EXPECT_EQ(GetStaticVertexIndex(Data, ShortMeshlet, i), Near[i]);
    }
    for (uint32 i = 0; i < 2u; ++i)
    {
        EXPECT_EQ(GetStaticVertexIndex(Data, LongMeshlet, i), Far[i]);
    }
}

// Two meshlets that each copied the same corner fold it back into one vertex.
TEST(MeshQuantization, LegacyStaticVerticesFoldIntoSharedVertices)
{
    const TVector<FVector3> Corners = { FVector3(0.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), FVector3(1.0f, 1.0f, 0.0f) };
    const uint32 MeshletCorners[2][3] = { { 0u, 1u, 2u }, { 1u, 3u, 2u } };

    TVector<FLegacyMeshlet>       LegacyMeshlets;
    TVector<FLegacyMeshletVertex> Legacy;
    for (const auto& Triangle : MeshletCorners)
    {
        LegacyMeshlets.push_back(MakeLegacyMeshlet((uint32)Legacy.size(), 3u));
        for (uint32 Corner : Triangle)
        {
            Legacy.push_back(MakeLegacyVertex(Corners[Corner]));
        }
    }

    FMeshletData Data;
    ConvertLegacyMeshletData(Data, LegacyMeshlets, Legacy, {});

    EXPECT_EQ(Data.VertexPositions.size(), 4u);
    EXPECT_TRUE(Data.VertexColors.empty());
    EXPECT_TRUE(Data.VertexUV1s.empty());
    for (uint32 m = 0; m < 2u; ++m)
    {
        EXPECT_EQ(Data.Meshlets[m].VertexCount, 3u);
        for (uint32 Local = 0; Local < 3u; ++Local)
        {
            const FVector3 P        = GetMeshletVertexPosition(Data, Data.Meshlets[m], Local, false);
            const FVector3 Expected = Corners[MeshletCorners[m][Local]];
            EXPECT_NEAR(P.x, Expected.x, 1e-5f);
            EXPECT_NEAR(P.y, Expected.y, 1e-5f);
            EXPECT_NEAR(P.z, Expected.z, 1e-5f);
        }
    }
}

// Skinned copies stay per meshlet but move onto the grid, and the meshlets stop carrying an anchor.
TEST(MeshQuantization, LegacySkinnedVerticesMoveOntoTheGrid)
{
    const TVector<FVector3> Corners = { FVector3(2.0f, 0.0f, 0.0f), FVector3(0.0f, 3.0f, 0.0f), FVector3(0.0f, 0.0f, 4.0f) };

    const TVector<FLegacyMeshlet> LegacyMeshlets = { MakeLegacyMeshlet(0u, 3u) };

    TVector<FLegacyMeshletSkinnedVertex> Legacy;
    for (const FVector3& Corner : Corners)
    {
        FLegacyMeshletSkinnedVertex V{};
        V.Base         = MakeLegacyVertex(Corner);
        V.JointIndices = 0x00010203u;
        V.JointWeights = 0x000000FFu;
        Legacy.push_back(V);
    }

    FMeshletData Data;
    ConvertLegacyMeshletData(Data, LegacyMeshlets, {}, Legacy);

    ASSERT_EQ(Data.MeshletSkinnedVertices.size(), 3u);
    EXPECT_EQ(Data.Meshlets[0].BaseVertex, 0u);
    for (uint32 Local = 0; Local < 3u; ++Local)
    {
        const FVector3 P = GetMeshletVertexPosition(Data, Data.Meshlets[0], Local, true);
        EXPECT_NEAR(P.x, Corners[Local].x, 1e-5f);
        EXPECT_NEAR(P.y, Corners[Local].y, 1e-5f);
        EXPECT_NEAR(P.z, Corners[Local].z, 1e-5f);
        EXPECT_EQ(Data.MeshletSkinnedVertices[Local].JointIndices, 0x00010203u);
    }
}
