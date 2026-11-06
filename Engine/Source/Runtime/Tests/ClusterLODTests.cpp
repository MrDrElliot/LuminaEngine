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
#include "Tools/Import/VoxelFarBuilder.h"
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

    void BuildClusterTerrain(FMeshResource& Resource, bool bTwoSurfaces, uint32 VoxelResolution)
    {
        BuildTerrain(Resource, bTwoSurfaces);
        Resource.bClusterLODs       = true;
        Resource.VoxelFarResolution = VoxelResolution;
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
    BuildClusterTerrain(Resource, false, 0u);

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
    BuildClusterTerrain(Resource, false, 0u);

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
    BuildClusterTerrain(Resource, false, 0u);

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

TEST(ClusterLOD, VoxelLevelsSitAboveTheHierarchy)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, true, 16u);

    ASSERT_EQ(Resource.GeometrySurfaces.Num(), 2);
    const FMeshletData& MD = Resource.MeshletData;
    ASSERT_EQ(MD.MeshletLODBounds.Num(), MD.Meshlets.Num());

    for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
    {
        uint32 VoxelSlots = 0;
        float  FirstVoxelError = -1.0f;
        for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
        {
            if (!Surface.IsVoxelLOD(Slot))
            {
                EXPECT_EQ(VoxelSlots, 0u) << *Surface.ID << " voxel slots must be the coarsest";
                continue;
            }
            if (VoxelSlots == 0u)
            {
                FirstVoxelError = Surface.LODError[Slot];
            }
            ++VoxelSlots;

            for (uint32 m = Surface.LODMeshletOffset[Slot]; m < Surface.LODMeshletOffset[Slot] + Surface.LODMeshletCount[Slot]; ++m)
            {
                EXPECT_NE(MD.MeshletLODBounds[m].Flags & MESHLET_LOD_FLAG_VOXEL, 0u) << *Surface.ID;
                EXPECT_EQ(MD.Meshlets[m].VertexCount, MD.Meshlets[m].TriangleCount * 3u) << *Surface.ID << " splats are lone triangles";
            }
        }
        ASSERT_GE(VoxelSlots, 2u) << *Surface.ID;

        // Every root hands over to the finest voxel level, so the triangles end where the splats begin.
        for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
        {
            if (Surface.IsVoxelLOD(Slot))
            {
                continue;
            }
            for (uint32 m = Surface.LODMeshletOffset[Slot]; m < Surface.LODMeshletOffset[Slot] + Surface.LODMeshletCount[Slot]; ++m)
            {
                const FMeshletLODBounds& B = MD.MeshletLODBounds[m];
                EXPECT_LT(B.ParentError, kMeshletLODErrorInfinite) << *Surface.ID;
                if ((B.Flags & MESHLET_LOD_FLAG_VOXEL_PARENT) != 0u)
                {
                    EXPECT_FLOAT_EQ(B.ParentError, FirstVoxelError) << *Surface.ID;
                }
            }
        }
    }
}

TEST(ClusterLOD, DiscreteLODsGainVoxelSlots)
{
    FMeshResource Resource;
    BuildTerrain(Resource, false);
    Resource.VoxelFarResolution = 16u;
    Import::Mesh::GenerateMeshlets(Resource);

    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    EXPECT_FALSE(Surface.HasClusterLOD());
    EXPECT_TRUE(Resource.MeshletData.MeshletLODBounds.IsEmpty());

    uint32 VoxelSlots = 0;
    for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
    {
        VoxelSlots += Surface.IsVoxelLOD(Slot) ? 1u : 0u;
        if (Slot > 0)
        {
            EXPECT_GE(Surface.LODError[Slot], Surface.LODError[Slot - 1]) << "Slot " << Slot;
        }
    }
    EXPECT_GE(VoxelSlots, 2u);
    EXPECT_TRUE(Surface.IsVoxelLOD(Surface.NumLODs - 1));
    EXPECT_FALSE(Surface.IsVoxelLOD(0));
}

namespace
{
    constexpr float  kVoxelGridSize     = 128.0f;
    constexpr uint32 kVoxelResolution   = 128u;

    // The terrain shrunk to half a voxel inside one finest voxel, and a second surface whose one triangle spans the grid.
    void BuildPatchOverEmptyVoxelLevel(FMeshResource& Resource, float& OutVoxelSize, FVector3& OutGridMin)
    {
        BuildTerrain(Resource, false);
        OutVoxelSize = kVoxelGridSize * 1.001f / (float)kVoxelResolution;
        OutGridMin   = FVector3(-kVoxelGridSize * 0.0005f);

        // Voxel (0, 0, 2) keeps its sparse splat, the one level up does not, and the levels past it do.
        const FVector3 SparseVoxel(0.0f, 0.0f, 2.0f);
        const FVector3 Center    = OutGridMin + (SparseVoxel + FVector3(0.5f)) * OutVoxelSize;
        const float    GridSpan  = (float)(kGridSize - 1u);
        const float    Shrink    = 0.5f * OutVoxelSize / GridSpan;
        for (FVector3& P : Resource.Positions)
        {
            P = Center + (P - FVector3(GridSpan * 0.5f, 0.0f, GridSpan * 0.5f)) * Shrink;
        }

        const uint32 Base = (uint32)Resource.Positions.Num();
        Resource.ResizeVertices((int32)Base + 3);
        const FVector3 Corners[3] = { FVector3(0.0f), FVector3(0.0f, 0.0f, kVoxelGridSize), FVector3(kVoxelGridSize, 0.0f, 0.0f) };
        for (uint32 c = 0; c < 3u; ++c)
        {
            Resource.Positions[Base + c] = Corners[c];
            Resource.Normals[Base + c]   = PackNormal(FVector3(0.0f, 1.0f, 0.0f));
            Resource.SetUVAt(Base + c, FVector2(0.0f));
            Resource.SetUV1At(Base + c, FVector2(0.0f));
            Resource.Colors[Base + c] = 0xFFFFFFFFu;
        }

        FGeometrySurface& Bounds = Resource.GeometrySurfaces.EmplaceGetRef();
        Bounds.ID            = "Bounds";
        Bounds.StartIndex    = (uint32)Resource.Indices.Num();
        Bounds.IndexCount    = 3u;
        Bounds.MaterialIndex = 1;
        Resource.Indices.Append({ Base, Base + 1u, Base + 2u });
    }
}

TEST(ClusterLOD, EmptyVoxelLevelLeavesNoSlotParentedToIt)
{
    for (bool bClusterLODs : { true, false })
    {
        FMeshResource Resource;
        float    VoxelSize = 0.0f;
        FVector3 GridMin(0.0f);
        BuildPatchOverEmptyVoxelLevel(Resource, VoxelSize, GridMin);

        const FGeometrySurface& Patch = Resource.GeometrySurfaces[0];
        TVector<float> Attributes(Resource.Positions.Num() * 5, 0.0f);
        for (int32 v = 0; v < Resource.Positions.Num(); ++v)
        {
            Attributes[v * 5 + 1] = 1.0f;
        }
        Import::VoxelFar::FBuildInput In;
        In.Indices    = TSpan<const uint32>(Resource.Indices.GetData(), Patch.IndexCount);
        In.Positions  = TSpan<const FVector3>(Resource.Positions.GetData(), Resource.Positions.Num());
        In.Attributes = TSpan<const float>(Attributes.GetData(), Attributes.Num());
        In.GridMin    = GridMin;
        In.VoxelSize  = VoxelSize;
        In.Resolution = kVoxelResolution;
        TVector<Import::VoxelFar::FLevel> Levels;
        Import::VoxelFar::Build(In, Levels);
        ASSERT_GE(Levels.Num(), 3);
        ASSERT_FALSE(Levels[0].Indices.IsEmpty());
        ASSERT_TRUE(Levels[1].Indices.IsEmpty()) << "the patch no longer leaves an intermediate voxel level empty";
        ASSERT_FALSE(Levels[2].Indices.IsEmpty());

        Resource.bClusterLODs       = bClusterLODs;
        Resource.VoxelFarResolution = kVoxelResolution;
        Import::Mesh::GenerateMeshlets(Resource);

        const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
        const FMeshletData&     MD      = Resource.MeshletData;
        uint32 VoxelSlots = 0;
        for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
        {
            if (!Surface.IsVoxelLOD(Slot))
            {
                continue;
            }
            ++VoxelSlots;
            const bool  bLast  = Slot + 1u == Surface.NumLODs;
            const float Parent = Surface.LODParentError[Slot];
            if (bLast)
            {
                EXPECT_GE(Parent, kMeshletLODErrorInfinite) << "cluster " << bClusterLODs;
            }
            else
            {
                EXPECT_FLOAT_EQ(Parent, Surface.LODError[Slot + 1u]) << "cluster " << bClusterLODs << " slot " << Slot;
            }
            for (uint32 m = Surface.LODMeshletOffset[Slot]; bClusterLODs && m < Surface.LODMeshletOffset[Slot] + Surface.LODMeshletCount[Slot]; ++m)
            {
                EXPECT_FLOAT_EQ(MD.MeshletLODBounds[m].ParentError, Parent) << "slot " << Slot;
            }
        }
        EXPECT_GE(VoxelSlots, 2u) << "cluster " << bClusterLODs;
    }
}

TEST(ClusterLOD, SurfaceSeamStaysSealedAtEveryLevel)
{
    FMeshResource Resource;
    BuildClusterTerrain(Resource, true, 0u);

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

TEST(ClusterLOD, CardCanopyHandsOverToVoxelsAtEveryDistance)
{
    FMeshResource Resource;
    BuildCardCanopy(Resource, 4000u, 10.0f, 0.3f);
    Resource.bClusterLODs       = true;
    Resource.VoxelFarResolution = 32u;
    Import::Mesh::GenerateMeshlets(Resource);

    const FMeshletData&     MD      = Resource.MeshletData;
    const FGeometrySurface& Surface = Resource.GeometrySurfaces[0];
    ASSERT_TRUE(Surface.HasClusterLOD());
    ASSERT_EQ(MD.MeshletLODBounds.Num(), MD.Meshlets.Num());

    uint32 VoxelSlots = 0;
    uint32 VoxelTriangles = 0;
    for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
    {
        if (Surface.IsVoxelLOD(Slot))
        {
            ++VoxelSlots;
            VoxelTriangles += TrianglesInRange(Resource, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot]);
        }
    }
    ASSERT_GE(VoxelSlots, 2u);
    EXPECT_GT(VoxelTriangles, 0u);

    // A splat level's group sphere is the surface's own, so it reaches no further than the canopy box does.
    const float BoxRadius = 10.0f * 0.8660254f;
    for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
    {
        const FMeshletLODBounds& B = MD.MeshletLODBounds[i];
        EXPECT_LE(B.SelfRadius, BoxRadius * 2.0f) << "meshlet " << i << (MD.Meshlets[i].bVoxel ? " voxel" : "");
        EXPECT_LE(B.ParentRadius, BoxRadius * 2.0f) << "meshlet " << i;
        EXPECT_LE(Math::Length(B.SelfCenter - FVector3(5.0f)), BoxRadius * 2.0f) << "meshlet " << i;
    }

    // Seen from a camera along +X, the perspective cut the GPU applies still hands the whole canopy to the splats far out.
    const float Distances[] = { 30.0f, 100.0f, 300.0f, 1000.0f, 5000.0f };
    for (float Distance : Distances)
    {
        const FVector3 Eye(Distance, 0.0f, 0.0f);
        const float ErrorScale = 50.0f;
        uint32 Triangles = 0, VoxelsDrawn = 0;
        for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
        {
            const FMeshletLODBounds& B = MD.MeshletLODBounds[i];
            const float SelfReach   = Math::Max(Math::Length(B.SelfCenter - Eye) - B.SelfRadius, 0.0f);
            const float ParentReach = Math::Max(Math::Length(B.ParentCenter - Eye) - B.ParentRadius, 0.0f);
            const bool  bSelf   = B.SelfError * ErrorScale <= SelfReach;
            const bool  bParent = B.ParentError >= kMeshletLODErrorInfinite || B.ParentError * ErrorScale > ParentReach;
            if (bSelf && bParent)
            {
                Triangles   += MD.Meshlets[i].TriangleCount;
                VoxelsDrawn += MD.Meshlets[i].bVoxel ? MD.Meshlets[i].TriangleCount : 0u;
            }
        }
        EXPECT_GT(Triangles, 0u) << "distance " << Distance;
        if (Distance >= 1000.0f)
        {
            EXPECT_EQ(Triangles, VoxelsDrawn) << "distance " << Distance << " should be splats only";
        }
    }

    // Every tolerance from a pixel of error up to the whole canopy picks something, and far enough out it is splats.
    const float Scales[] = { 1e3f, 1e2f, 1e1f, 1.0f, 0.3f, 0.1f };
    for (float ErrorScale : Scales)
    {
        uint32 Triangles      = 0;
        uint32 VoxelsDrawn    = 0;
        for (int32 i = 0; i < MD.Meshlets.Num(); ++i)
        {
            if (InCut(MD.MeshletLODBounds[i], ErrorScale))
            {
                Triangles   += MD.Meshlets[i].TriangleCount;
                VoxelsDrawn += MD.Meshlets[i].bVoxel ? MD.Meshlets[i].TriangleCount : 0u;
            }
        }
        EXPECT_GT(Triangles, 0u) << "scale " << ErrorScale;
        if (ErrorScale <= 0.1f)
        {
            EXPECT_GT(VoxelsDrawn, 0u) << "scale " << ErrorScale << " should have handed over to the splats";
        }
    }
}

// Cards collapse at almost no quadric cost, so without area preservation a coarse canopy level would be mostly air.
TEST(ClusterLOD, CardCanopyKeepsItsAreaThroughTheHierarchy)
{
    FMeshResource Resource;
    BuildCardCanopy(Resource, 4000u, 10.0f, 0.3f);
    Resource.bClusterLODs       = true;
    Resource.VoxelFarResolution = 0u;
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
    Resource.bClusterLODs = true;
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
