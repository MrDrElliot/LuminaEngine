#include "gtest/gtest.h"

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"

// Drives the real GenerateMeshlets, so meshopt runs through the LIFO bump arena on worker threads.

using namespace Lumina;

namespace
{
    constexpr float kGridStep = 1.0f;

    uint32 GridTriangleCount(uint32 N) { return (N - 1u) * (N - 1u) * 2u; }

    // Scrambling walks the quads by a coprime stride, destroying the locality a scan build would inherit.
    void BuildGrid(FMeshResource& Resource, uint32 N, bool bScramble)
    {
        const uint32 VertexCount = N * N;
        Resource.ResizeVertices(VertexCount);

        for (uint32 y = 0; y < N; ++y)
        {
            for (uint32 x = 0; x < N; ++x)
            {
                const uint32 i = y * N + x;
                Resource.Positions[i] = FVector3((float)x * kGridStep, 0.0f, (float)y * kGridStep);
                Resource.Normals[i]   = PackNormal(FVector3(0.0f, 1.0f, 0.0f));
                Resource.SetUVAt(i, FVector2((float)x / (float)(N - 1u), (float)y / (float)(N - 1u)));
                Resource.SetUV1At(i, FVector2(0.0f, 0.0f));
                Resource.Colors[i] = 0xFFFFFFFFu;
            }
        }

        const uint32 QuadCount = (N - 1u) * (N - 1u);
        const uint32 Stride    = bScramble ? 7919u : 1u;

        Resource.Indices.reserve(QuadCount * 6u);
        for (uint32 Step = 0; Step < QuadCount; ++Step)
        {
            const uint32 Quad = bScramble ? (uint32)(((uint64)Step * Stride) % QuadCount) : Step;
            const uint32 qx   = Quad % (N - 1u);
            const uint32 qy   = Quad / (N - 1u);

            const uint32 v00 = qy * N + qx;
            const uint32 v10 = v00 + 1u;
            const uint32 v01 = v00 + N;
            const uint32 v11 = v01 + 1u;

            Resource.Indices.push_back(v00);
            Resource.Indices.push_back(v01);
            Resource.Indices.push_back(v10);
            Resource.Indices.push_back(v10);
            Resource.Indices.push_back(v01);
            Resource.Indices.push_back(v11);
        }

        FGeometrySurface& Surface = Resource.GeometrySurfaces.emplace_back();
        Surface.ID            = "Grid";
        Surface.StartIndex    = 0;
        Surface.IndexCount    = (uint32)Resource.Indices.size();
        Surface.MaterialIndex = 0;
    }

    struct FLODZeroStats
    {
        uint32 MeshletCount  = 0;
        uint32 TriangleTotal = 0;
        uint32 MaxVertices   = 0;
        uint32 MaxTriangles  = 0;
        float  WorstSnap     = 0.0f;
    };

    FLODZeroStats GatherLODZero(const FMeshResource& Resource)
    {
        FLODZeroStats Stats;

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            const uint32 Begin = Surface.LODMeshletOffset[0];
            const uint32 Count = Surface.LODMeshletCount[0];
            Stats.MeshletCount += Count;

            for (uint32 m = Begin; m < Begin + Count; ++m)
            {
                const FMeshlet& Meshlet = Resource.MeshletData.Meshlets[m];
                Stats.TriangleTotal += Meshlet.TriangleCount;
                Stats.MaxVertices    = Math::Max(Stats.MaxVertices, Meshlet.VertexCount);
                Stats.MaxTriangles   = Math::Max(Stats.MaxTriangles, Meshlet.TriangleCount);

                for (uint32 i = 0; i < Meshlet.VertexCount; ++i)
                {
                    const FMeshletVertex& Vertex = Resource.MeshletData.MeshletVertices[Meshlet.VertexOffset + i];
                    const FVector3 P = DecodeMeshletPosition(Meshlet, Vertex);

                    const FVector3 Nearest(
                        Math::Round(P.x / kGridStep) * kGridStep,
                        0.0f,
                        Math::Round(P.z / kGridStep) * kGridStep);

                    Stats.WorstSnap = Math::Max(Stats.WorstSnap, Math::Length(P - Nearest));
                }
            }
        }

        return Stats;
    }

    FLODZeroStats BuildAndGather(uint32 N, bool bScramble, bool bFastMeshletBuild)
    {
        FMeshResource Resource;
        Resource.bFastMeshletBuild = bFastMeshletBuild;
        BuildGrid(Resource, N, bScramble);

        Import::Mesh::GenerateMeshlets(Resource);
        return GatherLODZero(Resource);
    }
}

TEST(MeshletBuild, GreedyCoversEveryTriangleWithinMeshletLimits)
{
    constexpr uint32 N = 48;
    const FLODZeroStats Stats = BuildAndGather(N, false, false);

    EXPECT_EQ(Stats.TriangleTotal, GridTriangleCount(N));
    EXPECT_LE(Stats.MaxVertices, (uint32)MESHLET_MAX_VERTICES);
    EXPECT_LE(Stats.MaxTriangles, (uint32)MESHLET_MAX_TRIANGLES);
}

TEST(MeshletBuild, ScanCoversEveryTriangleWithinMeshletLimits)
{
    constexpr uint32 N = 48;
    const FLODZeroStats Stats = BuildAndGather(N, false, true);

    EXPECT_EQ(Stats.TriangleTotal, GridTriangleCount(N));
    EXPECT_LE(Stats.MaxVertices, (uint32)MESHLET_MAX_VERTICES);
    EXPECT_LE(Stats.MaxTriangles, (uint32)MESHLET_MAX_TRIANGLES);
}

// Without the pre-pass a scrambled buffer hits the 64-vertex cap long before the 64-triangle one.
TEST(MeshletBuild, ScanPrePassKeepsMeshletCountNearGreedyOnScrambledInput)
{
    constexpr uint32 N = 48;

    const FLODZeroStats Greedy = BuildAndGather(N, true, false);
    const FLODZeroStats Scan   = BuildAndGather(N, true, true);

    EXPECT_EQ(Greedy.TriangleTotal, GridTriangleCount(N));
    EXPECT_EQ(Scan.TriangleTotal,   GridTriangleCount(N));

    ASSERT_GT(Greedy.MeshletCount, 0u);
    EXPECT_LE((float)Scan.MeshletCount, (float)Greedy.MeshletCount * 1.5f);
}

TEST(MeshletBuild, QuantizedPositionsRoundTripToTheSourceGrid)
{
    constexpr uint32 N = 48;

    for (bool bFast : { false, true })
    {
        const FLODZeroStats Stats = BuildAndGather(N, false, bFast);
        EXPECT_LT(Stats.WorstSnap, 0.01f) << "bFastMeshletBuild=" << bFast;
    }
}

// Runs past the per-thread meshopt arena, so the LIFO unwind interleaves arena and heap blocks.
TEST(MeshletBuild, LargeMeshExceedsTheMeshoptArenaAndStillBuilds)
{
    constexpr uint32 N = 200;

    for (bool bFast : { false, true })
    {
        const FLODZeroStats Stats = BuildAndGather(N, false, bFast);

        EXPECT_EQ(Stats.TriangleTotal, GridTriangleCount(N)) << "bFastMeshletBuild=" << bFast;
        EXPECT_LE(Stats.MaxVertices,  (uint32)MESHLET_MAX_VERTICES);
        EXPECT_LE(Stats.MaxTriangles, (uint32)MESHLET_MAX_TRIANGLES);
        EXPECT_LT(Stats.WorstSnap, 0.01f);
    }
}
