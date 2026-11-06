#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Renderer/MeshData.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Entity/Components/DynamicMeshComponent.h"

using namespace Lumina;

namespace
{
    // A capped trunk, the shape a chopped tree commits, with a UV seam column like a real bark mesh.
    FDynamicMeshBuildData MakeTrunk(uint32 Sides, uint32 Rings)
    {
        FDynamicMeshBuildData Data;
        const uint32 Columns = Sides + 1;
        for (uint32 Ring = 0; Ring <= Rings; ++Ring)
        {
            const float V = (float)Ring / (float)Rings;
            for (uint32 Column = 0; Column < Columns; ++Column)
            {
                const float U = (float)Column / (float)Sides;
                const float Angle = U * 6.2831853f;
                Data.Positions.Emplace(0.5f * std::cos(Angle), V * 10.0f, 0.5f * std::sin(Angle));
                Data.UVs.Emplace(U, V * 4.0f);
            }
        }
        for (uint32 Ring = 0; Ring < Rings; ++Ring)
        {
            for (uint32 Column = 0; Column < Sides; ++Column)
            {
                const uint32 A = Ring * Columns + Column;
                const uint32 B = A + 1;
                const uint32 C = A + Columns;
                const uint32 D = C + 1;
                Data.Indices.Append({ A, C, B, B, C, D });
            }
        }
        return Data;
    }

    struct FStageTimes
    {
        double BuildMs = 0.0;
        double MeshletsMs = 0.0;
    };

    FStageTimes TimeCommit(uint32 Sides, uint32 Rings, const FMeshBuildOptions& Options, int32 Repeats)
    {
        TVector<double> Build;
        TVector<double> Meshlets;
        for (int32 Run = 0; Run < Repeats; ++Run)
        {
            FDynamicMeshBuildData Data = MakeTrunk(Sides, Rings);
            const auto T0 = std::chrono::high_resolution_clock::now();
            TUniquePtr<FMeshResource> Resource = BuildMeshResource(Data, Options);
            const auto T1 = std::chrono::high_resolution_clock::now();
            Import::Mesh::GenerateMeshlets(*Resource);
            const auto T2 = std::chrono::high_resolution_clock::now();
            Build.Add(std::chrono::duration<double, std::milli>(T1 - T0).count());
            Meshlets.Add(std::chrono::duration<double, std::milli>(T2 - T1).count());
        }
        std::sort(Build.begin(), Build.end());
        std::sort(Meshlets.begin(), Meshlets.end());
        return { Build[Build.Num() / 2], Meshlets[Meshlets.Num() / 2] };
    }
}

// Run with --gtest_also_run_disabled_tests to see where a dynamic mesh commit spends its CPU time.
TEST(DynamicMeshCommitBench, DISABLED_StageCosts)
{
    struct FCase { const char* Name; bool bTangents; bool bOptimize; bool bFast; uint32 LODs; };
    const FCase Cases[] =
    {
        { "component defaults", true,  true,  true,  1 },
        { "no tangents",        false, true,  true,  1 },
        { "no meshlet optimize", true, false, true,  1 },
        { "neither",            false, false, true,  1 },
        { "full meshlet build", true,  true,  false, 1 },
    };
    const uint32 Sizes[][2] = { { 32, 64 }, { 48, 200 }, { 64, 512 } };

    for (const auto& Size : Sizes)
    {
        const uint32 Triangles = Size[0] * Size[1] * 2;
        for (const FCase& Case : Cases)
        {
            FMeshBuildOptions Options;
            Options.bGenerateTangents = Case.bTangents;
            Options.bOptimizeMeshlets = Case.bOptimize;
            Options.bFastMeshletBuild = Case.bFast;
            Options.MaxLODs = Case.LODs;
            const FStageTimes T = TimeCommit(Size[0], Size[1], Options, 7);
            std::printf("[bench] %6u tris  %-20s build %7.3f ms  meshlets %7.3f ms  total %7.3f ms\n",
                Triangles, Case.Name, T.BuildMs, T.MeshletsMs, T.BuildMs + T.MeshletsMs);
        }
    }
    SUCCEED();
}

// Large enough to take the chunked scan build and the parallel pack, which must still tile every triangle exactly once.
TEST(DynamicMeshCommit, ChunkedBuildCoversEveryTriangle)
{
    FDynamicMeshBuildData Data = MakeTrunk(64, 512);
    const size_t SourceTriangles = Data.Indices.Num() / 3;

    FMeshBuildOptions Options;
    Options.bGenerateTangents = true;
    TUniquePtr<FMeshResource> Resource = BuildMeshResource(Data, Options);
    Import::Mesh::GenerateMeshlets(*Resource);

    const FMeshletData& Meshlets = Resource->MeshletData;
    ASSERT_EQ(Resource->GeometrySurfaces.Num(), 1);
    const FGeometrySurface& Surface = Resource->GeometrySurfaces[0];
    EXPECT_EQ(Surface.LODMeshletOffset[0], 0u);
    EXPECT_EQ(Surface.LODMeshletCount[0], (uint32)Meshlets.Meshlets.Num());
    EXPECT_EQ(Meshlets.MeshletSpheres.Num(), Meshlets.Meshlets.Num());
    EXPECT_EQ(Meshlets.MeshletCones.Num(), Meshlets.Meshlets.Num());

    size_t Triangles   = 0;
    uint32 NextRefWord = 0;
    uint32 NextTriangle = 0;
    for (const FMeshlet& M : Meshlets.Meshlets)
    {
        EXPECT_EQ(M.VertexOffset, NextRefWord);
        EXPECT_EQ(M.TriangleOffset, NextTriangle);
        for (uint32 t = 0; t < M.TriangleCount; ++t)
        {
            const uint32 Packed = Meshlets.MeshletTriangles[M.TriangleOffset + t];
            EXPECT_LT(Packed & 0xFFu, M.VertexCount);
            EXPECT_LT((Packed >> 8) & 0xFFu, M.VertexCount);
            EXPECT_LT((Packed >> 16) & 0xFFu, M.VertexCount);
        }
        NextRefWord  += M.HasShortVertexRefs() ? (M.VertexCount + 1u) / 2u : M.VertexCount;
        NextTriangle += M.TriangleCount;
        Triangles    += M.TriangleCount;
    }
    EXPECT_EQ(Triangles, SourceTriangles);
    EXPECT_EQ(NextRefWord, static_cast<uint32>(Meshlets.MeshletVertexRefs.Num()));
    EXPECT_EQ(NextTriangle, static_cast<uint32>(Meshlets.MeshletTriangles.Num()));
}
