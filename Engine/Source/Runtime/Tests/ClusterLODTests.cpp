#include "gtest/gtest.h"

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include <cfloat>
#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ClusterLODBuilder.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

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

    void BuildClusterTerrain(FMeshResource& Resource, bool bTwoSurfaces)
    {
        BuildTerrain(Resource, bTwoSurfaces);
        Import::Mesh::GenerateMeshlets(Resource);
    }

    uint32 TrianglesInRange(const FMeshResource& Resource, uint32 Begin, uint32 Count)
    {
        uint32 Triangles = 0;
        for (uint32 m = Begin; m < Begin + Count; ++m)
        {
            Triangles += Resource.MeshletData.Meshlets[m].TriangleCount;
        }
        return Triangles;
    }

    // The GPU's cut for an orthographic view, where every reach is one and only the errors decide.
    bool InCut(const FMeshletLODBounds& B, float ErrorScale)
    {
        if (B.SelfError * ErrorScale > 1.0f)
        {
            return false;
        }
        return B.ParentError >= kMeshletLODErrorInfinite || B.ParentError * ErrorScale > 1.0f;
    }
}

TEST(ClusterLOD, BuildsAHierarchyWithBoundsPerMeshlet)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, false);

    const FMeshletData& MD = Resource.MeshletData;
    ASSERT_FALSE(MD.Meshlets.IsEmpty());
    ASSERT_EQ(MD.MeshletLODBounds.Num(), MD.Meshlets.Num());
    EXPECT_TRUE(Resource.HasConsistentMeshlets());

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    EXPECT_TRUE(Surface.HasClusterLOD());
    ASSERT_GE(Surface.NumLODs, 3u);

    // Slots are contiguous and finest first, which is what lets a view take one slice of them.
    for (uint32 Slot = 1; Slot < Surface.NumLODs; ++Slot)
    {
        EXPECT_EQ(Surface.LODMeshletOffset[Slot], Surface.LODMeshletOffset[Slot - 1] + Surface.LODMeshletCount[Slot - 1]) << "Slot " << Slot;
        EXPECT_GE(Surface.LODError[Slot], Surface.LODError[Slot - 1]) << "Slot " << Slot;
        EXPECT_GE(Surface.LODParentError[Slot], Surface.LODParentError[Slot - 1]) << "Slot " << Slot;
        EXPECT_LT(TrianglesInRange(Resource, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot]),
                  TrianglesInRange(Resource, Surface.LODMeshletOffset[0], Surface.LODMeshletCount[0])) << "Slot " << Slot;
    }
    EXPECT_EQ(Surface.LODError[0], 0.0f);
    EXPECT_GE(Surface.LODParentError[Surface.NumLODs - 1], kMeshletLODErrorInfinite);
}

TEST(ClusterLOD, EveryMeshletHasAParentCoarserThanItself)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, false);

    const FMeshletData& MD = Resource.MeshletData;
    uint32 Roots = 0;
    for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
    {
        const FMeshletLODBounds& B = MD.MeshletLODBounds[i];
        EXPECT_GE(B.SelfError, 0.0f);
        EXPECT_GT(B.ParentError, B.SelfError) << "meshlet " << i;
        EXPECT_EQ(B.Level, MD.Meshlets[i].LODIndex) << "meshlet " << i;
        if (B.Level == 0u)
        {
            EXPECT_EQ(B.SelfError, 0.0f) << "meshlet " << i;
        }
        Roots += B.ParentError >= kMeshletLODErrorInfinite ? 1u : 0u;
    }
    EXPECT_GT(Roots, 0u);
    EXPECT_LT(Roots, (uint32)MD.Meshlets.Num() / 4u);
}

TEST(ClusterLOD, OneCutCoversTheSurfaceOnceAtEveryTolerance)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, false);

    const FMeshletData&     MD      = Resource.MeshletData;
    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    const uint32 SourceTriangles    = TrianglesInRange(Resource, Surface.LODMeshletOffset[0], Surface.LODMeshletCount[0]);

    // Grouped by the error they were simplified from, every group's children are drawn together or not at all.
    const float Scales[] = { 0.0f, 1e6f, 1e3f, 1e2f, 1e1f, 1.0f, 1e-1f, 1e-2f };
    uint32 PreviousTriangles = ~0u;
    for (float ErrorScale : Scales)
    {
        uint32 Triangles = 0;
        for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
        {
            const FMeshletLODBounds& B = MD.MeshletLODBounds[i];
            const bool bDrawn = ErrorScale <= 0.0f ? B.SelfError == 0.0f : InCut(B, ErrorScale);
            Triangles += bDrawn ? MD.Meshlets[i].TriangleCount : 0u;
        }

        EXPECT_GT(Triangles, 0u) << "scale " << ErrorScale;
        EXPECT_LE(Triangles, SourceTriangles) << "scale " << ErrorScale;
        // A looser tolerance never draws more than a tighter one.
        EXPECT_LE(Triangles, PreviousTriangles) << "scale " << ErrorScale;
        PreviousTriangles = Triangles;
    }

    // At zero tolerance the cut is exactly the source geometry.
    uint32 FinestTriangles = 0;
    for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
    {
        FinestTriangles += MD.MeshletLODBounds[i].SelfError == 0.0f ? MD.Meshlets[i].TriangleCount : 0u;
    }
    EXPECT_EQ(FinestTriangles, SourceTriangles);
}

TEST(ClusterLOD, SurfaceSeamStaysSealedAtEveryLevel)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, true);

    // A locked seam vertex survives every level on both sides, so the two surfaces never open a crack.
    const FMeshletData& MD   = Resource.MeshletData;
    const uint32        Seam = kGridSize / 2u;
    for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
    {
        for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
        {
            TVector<bool> SeamRowPresent(kGridSize, false);
            for (uint32 m = Surface.LODMeshletOffset[Slot]; m < Surface.LODMeshletOffset[Slot] + Surface.LODMeshletCount[Slot]; ++m)
            {
                const FMeshlet& M = MD.Meshlets[m];
                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    const FVector3 P = GetMeshletVertexPosition(MD, M, v, false);
                    if (Math::Abs(P.x - (float)Seam) < 1e-3f)
                    {
                        const int32 Row = (int32)Math::Round(P.z);
                        if (Row >= 0 && Row < (int32)kGridSize)
                        {
                            SeamRowPresent[Row] = true;
                        }
                    }
                }
            }
            for (uint32 Row = 0; Row < kGridSize; ++Row)
            {
                EXPECT_TRUE(SeamRowPresent[Row]) << *Surface.ID << " slot " << Slot << " lost seam vertex " << Row;
            }
        }
    }
}

#endif

#if WITH_EDITOR
namespace
{
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
}

// Cards collapse at almost no quadric cost, so without area preservation a coarse canopy level would be mostly air.
TEST(ClusterLOD, CardCanopyKeepsItsAreaThroughTheHierarchy)
{
    FMeshResource Resource;
    BuildCardCanopy(Resource, 4000u, 10.0f, 0.3f);
    Import::Mesh::GenerateMeshlets(Resource);

    const FMeshletData&     MD      = Resource.MeshletData;
    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_TRUE(Surface.HasClusterLOD());
    ASSERT_GE(Surface.NumLODs, 3u);

    const double SourceArea = MeshResolve::MeshletRangeArea(MD, Surface.LODMeshletOffset[0], Surface.LODMeshletCount[0]);
    ASSERT_GT(SourceArea, 0.0);

    // A cut at any one slot stands for the whole surface, so every slot's triangles together must cover most of it.
    for (uint32 Slot = 1; Slot < Surface.NumLODs; ++Slot)
    {
        const uint32 Triangles = TrianglesInRange(Resource, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot]);
        const double Area      = MeshResolve::MeshletRangeArea(MD, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot]);
        EXPECT_LT(Triangles, TrianglesInRange(Resource, Surface.LODMeshletOffset[0], Surface.LODMeshletCount[0])) << "slot " << Slot;
        EXPECT_GT(Area, SourceArea * 0.5) << "slot " << Slot << " lost most of the canopy";
    }
}

namespace
{
    // Six faces of four corners each, so every face keeps its own normal.
    void BuildCube(FMeshResource& Resource, float HalfExtent)
    {
        Resource.ResizeVertices(24u);
        for (uint32 f = 0; f < 6u; ++f)
        {
            FVector3 Normal(0.0f);
            Normal[f / 2u] = (f & 1u) ? -1.0f : 1.0f;
            FVector3 U(0.0f);
            U[(f / 2u + 1u) % 3u] = 1.0f;
            const FVector3 V = Math::Cross(Normal, U);

            const FVector3 Corners[4] = { Normal - U - V, Normal + U - V, Normal + U + V, Normal - U + V };
            const FVector2 UVs[4]     = { FVector2(0.0f, 0.0f), FVector2(1.0f, 0.0f), FVector2(1.0f, 1.0f), FVector2(0.0f, 1.0f) };
            for (uint32 k = 0; k < 4u; ++k)
            {
                const uint32 i = f * 4u + k;
                Resource.Positions[i] = Corners[k] * HalfExtent;
                Resource.Normals[i]   = PackNormal(Normal);
                Resource.SetUVAt(i, UVs[k]);
                Resource.SetUV1At(i, FVector2(0.0f, 0.0f));
                Resource.Colors[i] = 0xFFFFFFFFu;
            }
            const uint32 b = f * 4u;
            Resource.Indices.Append({ b, b + 1u, b + 2u, b, b + 2u, b + 3u });
        }
        FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
        Surface.ID         = "Cube";
        Surface.IndexCount = (uint32)Resource.Indices.Num();
    }

    float SplatArea(const FInstanceProxy& Proxy)
    {
        return 4.0f * Proxy.SplatHalfSize * Proxy.SplatHalfSize;
    }
}

// A solid splat must cover about what the solid does on average, not the bounding square that read brighter past the transition.
TEST(ClusterLOD, SolidProxySplatMatchesItsMeanProjectedArea)
{
    constexpr float HalfExtent = 2.0f;
    FMeshResource Resource;
    BuildCube(Resource, HalfExtent);
    Import::Mesh::GenerateMeshlets(Resource);

    // Cauchy's formula, a quarter of the six faces' area.
    const FInstanceProxy Proxy = MeshResolve::ComputeSurfaceProxy(Resource, Resource.GeometrySurfaces[0]);
    const float MeanProjectedArea = 6.0f * HalfExtent * HalfExtent;
    const float BoundingRadius    = HalfExtent * 1.7320508f;
    EXPECT_NEAR(SplatArea(Proxy), MeanProjectedArea, MeanProjectedArea * 0.25f);
    EXPECT_LT(SplatArea(Proxy), 4.0f * BoundingRadius * BoundingRadius);
}

TEST(ClusterLOD, SparseCanopyProxySplatIsSmallerThanItsBoundingSquare)
{
    FMeshResource Resource;
    BuildCardCanopy(Resource, 4000u, 10.0f, 0.3f);
    Import::Mesh::GenerateMeshlets(Resource);

    const FInstanceProxy Proxy = MeshResolve::ComputeSurfaceProxy(Resource, Resource.GeometrySurfaces[0]);
    const float BoxRadius = 10.0f * 0.8660254f;
    EXPECT_GT(SplatArea(Proxy), 0.0f);
    EXPECT_LT(SplatArea(Proxy), 4.0f * BoxRadius * BoxRadius);
}

// Noise past MaxLevelError strands its groups as roots early, while the flat rest keeps simplifying along their open border.
TEST(ClusterLOD, StuckRootBorderStaysLockedAtEveryLaterLevel)
{
    constexpr uint32 N         = 64u;
    constexpr uint32 NoiseSize = 24u;
    TVector<FVector3> Positions;
    TVector<uint32>   Indices;
    uint32 Seed = 777u;
    for (uint32 z = 0; z < N; ++z)
    {
        for (uint32 x = 0; x < N; ++x)
        {
            Seed = Seed * 1664525u + 1013904223u;
            const float Noise = x < NoiseSize && z < NoiseSize ? (float)(Seed >> 8) / (float)(1u << 24) * 2.0f : 0.0f;
            Positions.Add(FVector3((float)x, Noise, (float)z));
        }
    }
    for (uint32 z = 0; z + 1u < N; ++z)
    {
        for (uint32 x = 0; x + 1u < N; ++x)
        {
            const uint32 v00 = z * N + x;
            Indices.Append({ v00, v00 + N, v00 + 1u, v00 + 1u, v00 + N, v00 + N + 1u });
        }
    }

    Import::ClusterLOD::FBuildInput In;
    In.Indices       = TSpan<const uint32>(Indices.GetData(), Indices.Num());
    In.Positions     = TSpan<const FVector3>(Positions.GetData(), Positions.Num());
    In.MaxLevelError = 0.3f;
    Import::ClusterLOD::FBuildOutput Out;
    ASSERT_TRUE(Import::ClusterLOD::Build(In, Out));
    ASSERT_GE(Out.NumLevels, 3u);

    auto MarkVertices = [&](uint32 Meshlet, TVector<bool>& Marks)
    {
        const FMeshlet& M = Out.Meshlets[Meshlet];
        for (uint32 v = 0; v < M.VertexCount; ++v)
        {
            Marks[Out.Vertices[M.VertexOffset + v]] = true;
        }
    };
    auto IsRoot = [&](uint32 Meshlet) { return Out.LODBounds[Meshlet].ParentError >= kMeshletLODErrorInfinite; };

    uint32 CheckedBorders = 0;
    for (uint32 Level = 0; Level + 1u < Out.NumLevels; ++Level)
    {
        TVector<bool> RootVertex(Out.Positions.Num(), false);
        TVector<bool> ParentedVertex(Out.Positions.Num(), false);
        for (uint32 m = Out.LevelBegin(Level); m < Out.LevelBegin(Level + 1u); ++m)
        {
            MarkVertices(m, IsRoot(m) ? RootVertex : ParentedVertex);
        }

        // Coarser neighbors are the later level's clusters plus the roots stranded on the way there.
        TVector<bool> Stranded(Out.Positions.Num(), false);
        for (uint32 Later = Level + 1u; Later < Out.NumLevels; ++Later)
        {
            TVector<bool> Coarser = Stranded;
            for (uint32 m = Out.LevelBegin(Later); m < Out.LevelBegin(Later + 1u); ++m)
            {
                MarkVertices(m, Coarser);
                if (IsRoot(m))
                {
                    MarkVertices(m, Stranded);
                }
            }
            for (int32 v = 0; v < Out.Positions.Num(); ++v)
            {
                if (RootVertex[v] && ParentedVertex[v])
                {
                    ++CheckedBorders;
                    EXPECT_TRUE(Coarser[v]) << "level " << Level << " root border vertex " << v << " lost at level " << Later;
                }
            }
        }
    }
    EXPECT_GT(CheckedBorders, 0u);
}
#endif
