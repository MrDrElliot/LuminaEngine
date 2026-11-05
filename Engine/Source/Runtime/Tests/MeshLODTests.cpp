#include "gtest/gtest.h"

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kGridSize = 96;

    float Height(float X, float Z)
    {
        return Math::Sin(X * 0.21f) * Math::Cos(Z * 0.17f) * 3.0f;
    }

    // A rolling height field, split into one surface per half when bTwoSurfaces, sharing the seam column's vertices.
    void BuildTerrain(FMeshResource& Resource, bool bTwoSurfaces)
    {
        const uint32 N = kGridSize;
        Resource.ResizeVertices(N * N);

        for (uint32 z = 0; z < N; ++z)
        {
            for (uint32 x = 0; x < N; ++x)
            {
                const uint32 i  = z * N + x;
                const float  fx = (float)x;
                const float  fz = (float)z;
                Resource.Positions[i] = FVector3(fx, Height(fx, fz), fz);

                const float    DX     = Height(fx + 0.5f, fz) - Height(fx - 0.5f, fz);
                const float    DZ     = Height(fx, fz + 0.5f) - Height(fx, fz - 0.5f);
                const FVector3 Normal = Math::Normalize(FVector3(-DX, 1.0f, -DZ));
                Resource.Normals[i] = PackNormal(Normal);
                Resource.SetUVAt(i, FVector2(fx / (float)(N - 1u), fz / (float)(N - 1u)));
                Resource.SetUV1At(i, FVector2(0.0f, 0.0f));
                Resource.Colors[i] = 0xFFFFFFFFu;
            }
        }

        const uint32 Seam = N / 2u;
        auto EmitQuads = [&](uint32 BeginX, uint32 EndX)
        {
            FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
            Surface.ID            = bTwoSurfaces ? (BeginX == 0 ? "Left" : "Right") : "Terrain";
            Surface.StartIndex    = (uint32)Resource.Indices.Num();
            Surface.MaterialIndex = (int16)(Resource.GeometrySurfaces.Num() - 1u);

            for (uint32 z = 0; z + 1u < N; ++z)
            {
                for (uint32 x = BeginX; x < EndX; ++x)
                {
                    const uint32 v00 = z * N + x;
                    const uint32 v10 = v00 + 1u;
                    const uint32 v01 = v00 + N;
                    const uint32 v11 = v01 + 1u;
                    Resource.Indices.Append({ v00, v01, v10, v10, v01, v11 });
                }
            }
            Surface.IndexCount = (uint32)Resource.Indices.Num() - Surface.StartIndex;
        };

        if (bTwoSurfaces)
        {
            EmitQuads(0u, Seam);
            EmitQuads(Seam, N - 1u);
        }
        else
        {
            EmitQuads(0u, N - 1u);
        }
    }

    uint32 TrianglesAtLOD(const FMeshResource& Resource, const FGeometrySurface& Surface, uint32 LOD)
    {
        uint32 Total = 0;
        for (uint32 m = 0; m < Surface.LODMeshletCount[LOD]; ++m)
        {
            Total += Resource.MeshletData.Meshlets[Surface.LODMeshletOffset[LOD] + m].TriangleCount;
        }
        return Total;
    }

    template<typename TVisitor>
    void ForEachPositionAtLOD(const FMeshResource& Resource, const FGeometrySurface& Surface, uint32 LOD, TVisitor&& Visit)
    {
        for (uint32 m = 0; m < Surface.LODMeshletCount[LOD]; ++m)
        {
            const FMeshlet& Meshlet = Resource.MeshletData.Meshlets[Surface.LODMeshletOffset[LOD] + m];
            for (uint32 v = 0; v < Meshlet.VertexCount; ++v)
            {
                Visit(GetMeshletVertexPosition(Resource.MeshletData, Meshlet, v, false));
            }
        }
    }

    // Leaf cards scattered through a canopy-sized box, each a separate quad sharing no vertex with any other.
    void BuildCardCanopy(FMeshResource& Resource, uint32 NumCards, float BoxSize, float CardSize)
    {
        Resource.ResizeVertices(NumCards * 4u);
        FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
        Surface.ID = "Canopy";

        uint32 Seed = 12345u;
        auto Next = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return (float)(Seed >> 8) / (float)(1u << 24); };

        for (uint32 c = 0; c < NumCards; ++c)
        {
            const FVector3 Center(Next() * BoxSize, Next() * BoxSize, Next() * BoxSize);
            const FVector3 Normal = Math::Normalize(FVector3(Next() - 0.5f, Next() - 0.5f, Next() - 0.5f) + FVector3(0.0f, 0.01f, 0.0f));
            const FVector3 Helper = Math::Abs(Normal.y) < 0.9f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 Right  = Math::Normalize(Math::Cross(Normal, Helper)) * (CardSize * 0.5f);
            const FVector3 Up     = Math::Normalize(Math::Cross(Right, Normal)) * (CardSize * 0.5f);

            const FVector3 Corners[4] = { Center - Right - Up, Center + Right - Up, Center + Right + Up, Center - Right + Up };
            const FVector2 UVs[4]     = { FVector2(0.0f, 0.0f), FVector2(1.0f, 0.0f), FVector2(1.0f, 1.0f), FVector2(0.0f, 1.0f) };
            for (uint32 k = 0; k < 4u; ++k)
            {
                const uint32 i = c * 4u + k;
                Resource.Positions[i] = Corners[k];
                Resource.Normals[i]   = PackNormal(Normal);
                Resource.SetUVAt(i, UVs[k]);
                Resource.SetUV1At(i, FVector2(0.0f, 0.0f));
                Resource.Colors[i] = 0xFFFFFFFFu;
            }

            const uint32 b = c * 4u;
            Resource.Indices.Append({ b, b + 1u, b + 2u, b, b + 2u, b + 3u });
        }
        Surface.IndexCount = (uint32)Resource.Indices.Num();
    }

    // Quantization moves a decoded position by a small fraction of the meshlet's extent.
    constexpr float kDecodeTolerance = 0.02f;
}

TEST(MeshLOD, ErrorRisesAsTrianglesFall)
{
    FMeshResource Resource;
    BuildTerrain(Resource, false);
    Import::Mesh::GenerateMeshlets(Resource);

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_GE(Surface.NumLODs, 3u);
    EXPECT_EQ(Surface.LODError[0], 0.0f);

    for (uint32 LOD = 1; LOD < Surface.NumLODs; ++LOD)
    {
        EXPECT_GT(Surface.LODError[LOD], 0.0f) << "LOD " << LOD;
        EXPECT_GE(Surface.LODError[LOD], Surface.LODError[LOD - 1]) << "LOD " << LOD;
        EXPECT_LT(TrianglesAtLOD(Resource, Surface, LOD), TrianglesAtLOD(Resource, Surface, LOD - 1)) << "LOD " << LOD;
    }
}

TEST(MeshLOD, SurfaceSeamStaysSealedAtEveryLOD)
{
    FMeshResource Resource;
    BuildTerrain(Resource, true);
    Import::Mesh::GenerateMeshlets(Resource);

    ASSERT_EQ(Resource.GeometrySurfaces.Num(), 2);
    const float SeamX = (float)(kGridSize / 2u);

    for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
    {
        ASSERT_GE(Surface.NumLODs, 2u) << *Surface.ID;

        for (uint32 LOD = 0; LOD < Surface.NumLODs; ++LOD)
        {
            TVector<bool> SeamRowPresent(kGridSize, false);
            ForEachPositionAtLOD(Resource, Surface, LOD, [&](const FVector3& P)
            {
                if (Math::Abs(P.x - SeamX) < kDecodeTolerance)
                {
                    const int32 Row = (int32)Math::Round(P.z);
                    if (Row >= 0 && Row < (int32)kGridSize && Math::Abs(P.z - (float)Row) < kDecodeTolerance)
                    {
                        SeamRowPresent[Row] = true;
                    }
                }
            });

            for (uint32 Row = 0; Row < kGridSize; ++Row)
            {
                EXPECT_TRUE(SeamRowPresent[Row]) << *Surface.ID << " LOD " << LOD << " lost seam vertex " << Row;
            }
        }
    }
}

TEST(MeshLOD, NonDestructiveLevelsOnlyReuseSourceVertices)
{
    FMeshResource Resource;
    Resource.bDestructiveLODs = false;
    BuildTerrain(Resource, false);
    Import::Mesh::GenerateMeshlets(Resource);

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_GE(Surface.NumLODs, 2u);

    for (uint32 LOD = 1; LOD < Surface.NumLODs; ++LOD)
    {
        ForEachPositionAtLOD(Resource, Surface, LOD, [&](const FVector3& P)
        {
            const FVector3 Grid = FVector3(Math::Round(P.x), 0.0f, Math::Round(P.z));
            EXPECT_NEAR(P.x, Grid.x, kDecodeTolerance);
            EXPECT_NEAR(P.z, Grid.z, kDecodeTolerance);
        });
    }
}

TEST(MeshLOD, DestructiveLevelsMoveVertices)
{
    FMeshResource Resource;
    BuildTerrain(Resource, false);
    Import::Mesh::GenerateMeshlets(Resource);

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_GE(Surface.NumLODs, 2u);

    uint32 MovedVertices = 0;
    ForEachPositionAtLOD(Resource, Surface, Surface.NumLODs - 1u, [&](const FVector3& P)
    {
        const bool bOnGrid = Math::Abs(P.x - Math::Round(P.x)) < kDecodeTolerance
                          && Math::Abs(P.z - Math::Round(P.z)) < kDecodeTolerance;
        MovedVertices += bOnGrid ? 0u : 1u;
    });

    EXPECT_GT(MovedVertices, 0u);
}

// Collapsing a card leaves a point on its own plane, so a plane-distance error reads near zero while the canopy thins.
TEST(MeshLOD, CardFoliageErrorKeepsRisingAcrossLevels)
{
    FMeshResource Resource;
    BuildCardCanopy(Resource, 4000u, 10.0f, 0.5f);
    Import::Mesh::GenerateMeshlets(Resource);

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_GE(Surface.NumLODs, 3u);

    for (uint32 LOD = 2; LOD < Surface.NumLODs; ++LOD)
    {
        EXPECT_GT(Surface.LODError[LOD], Surface.LODError[LOD - 1] * 1.1f) << "LOD " << LOD;
    }
}

#endif
