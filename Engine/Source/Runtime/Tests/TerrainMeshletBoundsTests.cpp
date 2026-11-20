#include <gtest/gtest.h>

#include "Core/Math/Math.h"
#include "Core/Templates/NumericLimits.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Scene/RenderScene/TerrainMeshletBuilder.h"

#include <cmath>

using namespace Lumina;

namespace
{
    // The per-sample clamp the vectorized reduction replaced, kept here as the reference to judge it.
    float SampleClamped(const TVector<float>& Heightmap, int32 X, int32 Y, int32 Resolution, float MaxHeight)
    {
        X = Math::Clamp(X, 0, Resolution - 1);
        Y = Math::Clamp(Y, 0, Resolution - 1);
        return Heightmap[Y * Resolution + X] * MaxHeight;
    }

    STerrainComponent MakeTerrain(int32 Resolution, int32 ChunkResolution, float MaxHeight)
    {
        STerrainComponent Terrain;
        Terrain.Resolution       = Resolution;
        Terrain.ChunkResolution  = ChunkResolution;
        Terrain.TileWorldSize    = 128.0f;
        Terrain.MaxHeight        = MaxHeight;
        Terrain.Heightmap.SetNum(Lumina::IntCastChecked<Lumina::int32>(static_cast<int64>(Resolution) * Resolution));

        for (int32 y = 0; y < Resolution; ++y)
        {
            for (int32 x = 0; x < Resolution; ++x)
            {
                // Uncorrelated in x and y so a transposed or off-by-one read cannot pass.
                const float V = std::sin((float)x * 0.31f) * std::cos((float)y * 0.17f) + (float)((x * 7 + y * 13) % 11) * 0.05f;
                Terrain.Heightmap[y * Resolution + x] = V;
            }
        }
        return Terrain;
    }

    void ExpectBoundsMatchScalarReference(int32 Resolution, int32 ChunkResolution, float MaxHeight)
    {
        STerrainComponent Terrain = MakeTerrain(Resolution, ChunkResolution, MaxHeight);
        const FVector3 WorldOrigin(0.0f, 0.0f, 0.0f);

        TerrainMeshletBuilder::Build(Terrain, WorldOrigin);

        const FTerrainCPUState& State = Terrain.CPUState;
        ASSERT_FALSE(State.Meshlets.IsEmpty()) << "builder produced no meshlets";

        const int32 QuadsPerChunk = ChunkResolution - 1;
        const int32 MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;
        const float DilateY = Math::Max(0.05f, MaxHeight * 0.01f);

        for (int32 m = 0; m < State.Meshlets.Num(); ++m)
        {
            const FTerrainMeshletInfo& Meshlet = State.Meshlets[m];
            const FTerrainChunkInfo& Chunk = State.Chunks[Meshlet.ChunkIndex];

            // A coarser level's quads span 1 << Level samples, the last one clamped to the chunk's own extent.
            const FIntVector2 Extent = GetTerrainChunkExtent(Chunk.QuadOrigin, QuadsPerChunk, Resolution);
            const int32 FineX0   = Meshlet.ChunkLocalQuadOrigin.x << Meshlet.Level;
            const int32 FineY0   = Meshlet.ChunkLocalQuadOrigin.y << Meshlet.Level;
            const int32 FineX1   = Math::Min((Meshlet.ChunkLocalQuadOrigin.x + Meshlet.QuadExtent.x) << Meshlet.Level, Extent.x);
            const int32 FineY1   = Math::Min((Meshlet.ChunkLocalQuadOrigin.y + Meshlet.QuadExtent.y) << Meshlet.Level, Extent.y);
            const int32 SampleX0 = Chunk.QuadOrigin.x + FineX0;
            const int32 SampleY0 = Chunk.QuadOrigin.y + FineY0;
            const int32 NVertsX  = FineX1 - FineX0 + 1;
            const int32 NVertsY  = FineY1 - FineY0 + 1;

            float WantMin =  TNumericLimits<float>::Infinity();
            float WantMax = -TNumericLimits<float>::Infinity();
            for (int32 vy = 0; vy < NVertsY; ++vy)
            {
                for (int32 vx = 0; vx < NVertsX; ++vx)
                {
                    const float H = SampleClamped(Terrain.Heightmap, SampleX0 + vx, SampleY0 + vy, Resolution, MaxHeight);
                    WantMin = Math::Min(WantMin, H);
                    WantMax = Math::Max(WantMax, H);
                }
            }

            EXPECT_FLOAT_EQ(Meshlet.BoundsMin.y, WantMin - DilateY)
                << "meshlet " << m << " min at (" << SampleX0 << "," << SampleY0 << ")";
            EXPECT_FLOAT_EQ(Meshlet.BoundsMax.y, WantMax + DilateY)
                << "meshlet " << m << " max at (" << SampleX0 << "," << SampleY0 << ")";
        }

        EXPECT_GT(MeshletsPerChunkSide, 0);
    }
}

TEST(TerrainMeshletBounds, MatchesScalarReferenceOnAlignedGrid)
{
    // 8 quads per chunk over 7-quad meshlets, so every chunk has a partial meshlet on two sides.
    ExpectBoundsMatchScalarReference(65, 9, 100.0f);
}

TEST(TerrainMeshletBounds, MatchesScalarReferenceWhenMeshletsRunPastTheEdge)
{
    // Resolution-1 is not a whole number of chunks, so the last chunk is narrower than the rest.
    ExpectBoundsMatchScalarReference(50, 17, 100.0f);
}

TEST(TerrainMeshletBounds, MatchesScalarReferenceWithSpanBelowEightSamples)
{
    // Chunks of 4 quads keep every meshlet span under the 8-wide step, so only the scalar tail runs.
    ExpectBoundsMatchScalarReference(33, 5, 100.0f);
}

TEST(TerrainMeshletBounds, MatchesScalarReferenceWithNegativeMaxHeight)
{
    // A negative scale flips which end of the raw range becomes the minimum.
    ExpectBoundsMatchScalarReference(65, 9, -75.0f);
}

TEST(TerrainMeshletBounds, ChunksCoverEveryQuad)
{
    // 1024 quads in 63-quad chunks leaves a remainder, which once went undrawn as a seam along two edges.
    STerrainComponent Terrain = MakeTerrain(1025, 64, 100.0f);
    TerrainMeshletBuilder::Build(Terrain, FVector3(0.0f));

    int32 FarX = 0;
    int32 FarY = 0;
    for (const FTerrainMeshletInfo& Meshlet : Terrain.CPUState.Meshlets)
    {
        const FTerrainChunkInfo& Chunk = Terrain.CPUState.Chunks[Meshlet.ChunkIndex];
        FarX = Math::Max(FarX, Chunk.QuadOrigin.x + Meshlet.ChunkLocalQuadOrigin.x + Meshlet.QuadExtent.x);
        FarY = Math::Max(FarY, Chunk.QuadOrigin.y + Meshlet.ChunkLocalQuadOrigin.y + Meshlet.QuadExtent.y);
    }
    EXPECT_GE(FarX, 1024);
    EXPECT_GE(FarY, 1024);
}

TEST(TerrainMeshletBounds, TranslateMatchesRebuildAtNewOrigin)
{
    const FVector3 From(10.0f, -3.0f, 7.0f);
    const FVector3 To(-250.0f, 40.0f, 512.0f);

    STerrainComponent Moved = MakeTerrain(65, 9, 50.0f);
    TerrainMeshletBuilder::Build(Moved, From);
    TerrainMeshletBuilder::Translate(Moved, To - From);

    STerrainComponent Rebuilt = MakeTerrain(65, 9, 50.0f);
    TerrainMeshletBuilder::Build(Rebuilt, To);

    EXPECT_EQ(Moved.CPUState.PreparedOrigin, To);
    ASSERT_EQ(Moved.CPUState.Chunks.Num(), Rebuilt.CPUState.Chunks.Num());
    ASSERT_EQ(Moved.CPUState.Meshlets.Num(), Rebuilt.CPUState.Meshlets.Num());
    for (int32 i = 0; i < Moved.CPUState.Meshlets.Num(); ++i)
    {
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            EXPECT_NEAR(Moved.CPUState.Meshlets[i].BoundsMin[Axis], Rebuilt.CPUState.Meshlets[i].BoundsMin[Axis], 1e-3f);
            EXPECT_NEAR(Moved.CPUState.Meshlets[i].BoundsMax[Axis], Rebuilt.CPUState.Meshlets[i].BoundsMax[Axis], 1e-3f);
        }
    }
    for (int32 i = 0; i < Moved.CPUState.Chunks.Num(); ++i)
    {
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            EXPECT_NEAR(Moved.CPUState.Chunks[i].BoundsMin[Axis], Rebuilt.CPUState.Chunks[i].BoundsMin[Axis], 1e-3f);
            EXPECT_NEAR(Moved.CPUState.Chunks[i].BoundsMax[Axis], Rebuilt.CPUState.Chunks[i].BoundsMax[Axis], 1e-3f);
        }
    }
}

TEST(TerrainMeshletBounds, EveryLevelCoversItsWholeChunk)
{
    // 49 quads in 16-quad chunks leaves a narrow last chunk, whose clamped coarse quads must still reach its edge.
    STerrainComponent Terrain = MakeTerrain(50, 17, 100.0f);
    TerrainMeshletBuilder::Build(Terrain, FVector3(0.0f));
    const FTerrainCPUState& State = Terrain.CPUState;

    for (int32 c = 0; c < State.Chunks.Num(); ++c)
    {
        const FTerrainChunkInfo& Chunk = State.Chunks[c];
        const FIntVector2 Extent = GetTerrainChunkExtent(Chunk.QuadOrigin, 16, 50);
        for (uint32 Level = 0; Level < GTerrainLODLevels; ++Level)
        {
            int32 FarX = 0;
            int32 FarY = 0;
            int32 Count = 0;
            for (uint32 m = Chunk.MeshletOffset; m < Chunk.MeshletOffset + Chunk.MeshletCount; ++m)
            {
                const FTerrainMeshletInfo& Meshlet = State.Meshlets[m];
                if (Meshlet.Level != Level)
                {
                    continue;
                }
                ++Count;
                FarX = Math::Max(FarX, Math::Min((Meshlet.ChunkLocalQuadOrigin.x + Meshlet.QuadExtent.x) << Level, Extent.x));
                FarY = Math::Max(FarY, Math::Min((Meshlet.ChunkLocalQuadOrigin.y + Meshlet.QuadExtent.y) << Level, Extent.y));
            }
            EXPECT_EQ(Count, GetTerrainLevelMeshletsSide(Extent.x, Level) * GetTerrainLevelMeshletsSide(Extent.y, Level)) << "chunk " << c << " level " << Level;
            EXPECT_EQ(FarX, Extent.x) << "chunk " << c << " level " << Level;
            EXPECT_EQ(FarY, Extent.y) << "chunk " << c << " level " << Level;
        }
    }
}
