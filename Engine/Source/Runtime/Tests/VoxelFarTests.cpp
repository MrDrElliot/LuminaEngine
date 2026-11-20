#include "gtest/gtest.h"

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Core/Math/Math.h"
#include "Tools/Import/VoxelFarBuilder.h"
#include "Renderer/MeshQuantization.h"
#include "Tools/Import/ImportHelpers.h"

using namespace Lumina;

namespace
{
    struct FSplatMesh
    {
        TVector<FVector3> Positions;
        TVector<float>    Attributes;
        TVector<uint32>   Indices;

        // Wound counterclockwise about U cross V, with Normal as every corner's shading normal.
        void AddQuad(const FVector3& Center, const FVector3& Normal, const FVector3& U, const FVector3& V, float HalfSize)
        {
            const uint32 Base = (uint32)Positions.Num();
            for (int32 Corner = 0; Corner < 4; ++Corner)
            {
                const float CU = (Corner & 1) ? 1.0f : -1.0f;
                const float CV = (Corner & 2) ? 1.0f : -1.0f;
                Positions.Add(Center + (U * CU + V * CV) * HalfSize);
                Attributes.Append({ Normal.x, Normal.y, Normal.z, CU * 0.5f + 0.5f, CV * 0.5f + 0.5f });
            }
            Indices.Append({ Base, Base + 1u, Base + 2u, Base + 2u, Base + 1u, Base + 3u });
        }

        // A cube with a quad per face, so the splats take every tangent frame the axis normals produce.
        void AddCube()
        {
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                for (float Side : { -1.0f, 1.0f })
                {
                    FVector3 Normal(0.0f);
                    Normal[Axis] = Side;
                    FVector3 U(0.0f);
                    U[(Axis + 1) % 3] = 1.0f;
                    FVector3 V(0.0f);
                    V[(Axis + 2) % 3] = 1.0f;
                    AddQuad(FVector3(8.0f) + Normal * 7.3f, Normal, U, V, 7.3f);
                }
            }
        }

        TVector<Import::VoxelFar::FLevel> Build(Import::VoxelFar::FBuildInput& In) const
        {
            In.Indices    = TSpan<const uint32>(Indices.GetData(), Indices.Num());
            In.Positions  = TSpan<const FVector3>(Positions.GetData(), Positions.Num());
            In.Attributes = TSpan<const float>(Attributes.GetData(), Attributes.Num());
            In.GridMin    = FVector3(0.0f);
            In.VoxelSize  = 0.5f;
            In.Resolution = 32u;

            TVector<Import::VoxelFar::FLevel> Levels;
            Import::VoxelFar::Build(In, Levels);
            return Levels;
        }
    };

    FVector3 SplatNormal(const Import::VoxelFar::FLevel& Level, int32 Tri)
    {
        const float* Attr = &Level.Attributes[Level.Indices[Tri] * 5u];
        return FVector3(Attr[0], Attr[1], Attr[2]);
    }

    FVector3 SplatWindingNormal(const Import::VoxelFar::FLevel& Level, int32 Tri)
    {
        const FVector3& A = Level.Positions[Level.Indices[Tri]];
        const FVector3& B = Level.Positions[Level.Indices[Tri + 1]];
        const FVector3& C = Level.Positions[Level.Indices[Tri + 2]];
        return Math::Cross(B - A, C - A);
    }
}

TEST(VoxelFar, EverySplatCornerLiesWithinTheLevelErrorOfItsVoxelCenter)
{
    FSplatMesh Mesh;
    Mesh.AddCube();
    Import::VoxelFar::FBuildInput In;
    const TVector<Import::VoxelFar::FLevel> Levels = Mesh.Build(In);
    ASSERT_FALSE(Levels.IsEmpty());

    for (const Import::VoxelFar::FLevel& Level : Levels)
    {
        ASSERT_GT(Level.Indices.Num(), 0);
        for (int32 Tri = 0; Tri + 2 < Level.Indices.Num(); Tri += 3)
        {
            const FVector3 Corners[3] = { Level.Positions[Level.Indices[Tri]], Level.Positions[Level.Indices[Tri + 1]], Level.Positions[Level.Indices[Tri + 2]] };
            // A splat is centered on its voxel, so the corners' centroid recovers the voxel center.
            const FVector3 Center = (Corners[0] + Corners[1] + Corners[2]) / 3.0f;
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                const float Cell = (Center[Axis] - In.GridMin[Axis]) / Level.VoxelSize - 0.5f;
                EXPECT_NEAR(Cell, Math::Round(Cell), 1e-3f);
            }
            for (const FVector3& Corner : Corners)
            {
                EXPECT_LE(Math::Length(Corner - Center), Level.Error * 1.0001f);
            }
        }
    }
}

// The scene raster takes clockwise framebuffer faces as front because the projection flips Y (OpenSceneCommandList in DefaultSceneRenderer.cpp), so world space counterclockwise is front.
TEST(VoxelFar, SplatWindingFacesTheSameWayAsItsShadingNormal)
{
    FSplatMesh Mesh;
    Mesh.AddCube();
    Import::VoxelFar::FBuildInput In;
    const TVector<Import::VoxelFar::FLevel> Levels = Mesh.Build(In);
    ASSERT_FALSE(Levels.IsEmpty());

    for (const Import::VoxelFar::FLevel& Level : Levels)
    {
        for (int32 Tri = 0; Tri + 2 < Level.Indices.Num(); Tri += 3)
        {
            const FVector3 Winding = Math::Normalize(SplatWindingNormal(Level, Tri));
            EXPECT_GT(Math::Dot(Winding, SplatNormal(Level, Tri)), 0.999f);
        }
    }
}

TEST(VoxelFar, ATwoSidedQuadKeepsItsPlaneAndFacesBothWays)
{
    const FVector3 X(1.0f, 0.0f, 0.0f);
    const FVector3 Y(0.0f, 1.0f, 0.0f);
    const FVector3 Z(0.0f, 0.0f, 1.0f);
    FSplatMesh Mesh;
    Mesh.AddQuad(FVector3(8.0f, 8.0f, 8.1f), Z, X, Y, 6.0f);
    Mesh.AddQuad(FVector3(8.0f, 8.0f, 8.1f), -Z, Y, X, 6.0f);
    Import::VoxelFar::FBuildInput In;
    const TVector<Import::VoxelFar::FLevel> Levels = Mesh.Build(In);
    ASSERT_FALSE(Levels.IsEmpty());

    for (const Import::VoxelFar::FLevel& Level : Levels)
    {
        ASSERT_GT(Level.VoxelCount, 0u);
        EXPECT_EQ((uint32)Level.Indices.Num(), Level.VoxelCount * 6u);
        for (int32 Tri = 0; Tri + 2 < Level.Indices.Num(); Tri += 6)
        {
            const FVector3 Front = SplatNormal(Level, Tri);
            EXPECT_GT(Math::Abs(Front.z), 0.999f);
            EXPECT_LT(Math::Dot(SplatNormal(Level, Tri + 3), Front), -0.999f);
            EXPECT_GT(Math::Dot(Math::Normalize(SplatWindingNormal(Level, Tri)), Front), 0.999f);
            EXPECT_GT(Math::Dot(Math::Normalize(SplatWindingNormal(Level, Tri + 3)), -Front), 0.999f);
        }
    }
}

TEST(VoxelFar, SharedSurfacesDrawEachVoxelOnce)
{
    // A trunk inside a crown, as two surfaces filling the same voxels.
    FSplatMesh Crown;
    Crown.AddQuad(FVector3(8.0f, 8.0f, 8.2f), FVector3(0.0f, 0.0f, 1.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), 6.0f);
    FSplatMesh Trunk;
    Trunk.AddQuad(FVector3(8.0f, 8.0f, 8.2f), FVector3(0.0f, 0.0f, 1.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), 2.0f);

    Import::VoxelFar::FBuildInput Inputs[2];
    const FSplatMesh* Meshes[2] = { &Crown, &Trunk };
    TVector<Import::VoxelFar::FSurfaceVoxels> Voxels(2);
    for (int32 Surface = 0; Surface < 2; ++Surface)
    {
        Import::VoxelFar::FBuildInput& In = Inputs[Surface];
        In.Indices    = TSpan<const uint32>(Meshes[Surface]->Indices.GetData(), Meshes[Surface]->Indices.Num());
        In.Positions  = TSpan<const FVector3>(Meshes[Surface]->Positions.GetData(), Meshes[Surface]->Positions.Num());
        In.Attributes = TSpan<const float>(Meshes[Surface]->Attributes.GetData(), Meshes[Surface]->Attributes.Num());
        In.VoxelSize  = 0.5f;
        In.Resolution = 32u;
        Import::VoxelFar::Voxelize(In, Voxels[Surface]);
    }
    Import::VoxelFar::FSharedVoxels Shared;
    Import::VoxelFar::Share(TSpan<const Import::VoxelFar::FSurfaceVoxels>(Voxels.GetData(), Voxels.Num()), Shared);

    TVector<Import::VoxelFar::FLevel> Levels[2];
    for (uint32 Surface = 0; Surface < 2u; ++Surface)
    {
        Import::VoxelFar::Emit(Inputs[Surface], Voxels[Surface], &Shared, Surface, Levels[Surface]);
    }

    THashSet<uint64> Centers;
    int32 Splats = 0;
    for (const TVector<Import::VoxelFar::FLevel>& SurfaceLevels : Levels)
    {
        const Import::VoxelFar::FLevel& Finest = SurfaceLevels[0];
        for (int32 Tri = 0; Tri + 2 < Finest.Indices.Num(); Tri += 3)
        {
            const FVector3 Center = (Finest.Positions[Finest.Indices[Tri]] + Finest.Positions[Finest.Indices[Tri + 1]] + Finest.Positions[Finest.Indices[Tri + 2]]) / 3.0f;
            const uint64 Cell = ((uint64)Math::Floor(Center.x / Finest.VoxelSize) << 40) | ((uint64)Math::Floor(Center.y / Finest.VoxelSize) << 20) | (uint64)Math::Floor(Center.z / Finest.VoxelSize);
            EXPECT_TRUE(Centers.TryAdd(Cell)) << "two surfaces drew one voxel";
            ++Splats;
        }
    }
    EXPECT_GT(Splats, 0);
}

TEST(VoxelFar, CompactSplatsKeepEveryCenterReachAndFacing)
{
    // A cube built as an aggregate is, voxel levels alone, so every meshlet is splats.
    FSplatMesh Cube;
    Cube.AddCube();
    FMeshResource Resource;
    Resource.ResizeVertices(Cube.Positions.Num());
    for (int32 v = 0; v < Cube.Positions.Num(); ++v)
    {
        const float* Attr = &Cube.Attributes[v * 5];
        Resource.Positions[v] = Cube.Positions[v];
        Resource.Normals[v]   = PackNormal(FVector3(Attr[0], Attr[1], Attr[2]));
        Resource.Tangents[v]  = PackTangent(FVector3(1.0f, 0.0f, 0.0f), 1.0f);
        Resource.UVs[v]       = Math::PackHalf2x16(FVector2(Attr[3], Attr[4]));
        Resource.UVs1[v]      = Resource.UVs[v];
        Resource.Colors[v]    = 0xFFFFFFFFu;
    }
    Resource.Indices = Cube.Indices;
    FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
    Surface.IndexCount    = (uint32)Cube.Indices.Num();
    Surface.MaterialIndex = 0;
    Resource.bGenerateTangents  = false;
    Resource.bClusterLODs       = false;
    Resource.MaxLODs            = 1;
    Resource.VoxelFarResolution = 32;
    Resource.bVoxelLevelsOnly   = true;
    Import::Mesh::GenerateMeshlets(Resource);
    ASSERT_FALSE(Resource.MeshletData.IsEmpty());

    const FMeshletData Before = Resource.MeshletData;
    FMeshletData After = Resource.MeshletData;
    ASSERT_TRUE(Import::Mesh::CompactSplatMeshlets(After));
    EXPECT_LT(After.VertexPositions.Num() * 3, Before.VertexPositions.Num() + 3);

    // A loaded aggregate is validated before it uploads, so the compact layout has to pass the same check.
    FMeshResource Loaded;
    Loaded.GeometrySurfaces = Resource.GeometrySurfaces;
    Loaded.MeshletData      = After;
    EXPECT_TRUE(Loaded.HasConsistentMeshlets());

    const float GridStep = PositionGridScale(Before.PositionGrid.Exponent);
    for (int32 m = 0; m < Before.Meshlets.Num(); ++m)
    {
        const FMeshlet& Old = Before.Meshlets[m];
        const FMeshlet& New = After.Meshlets[m];
        ASSERT_EQ(Old.TriangleCount, New.TriangleCount);
        ASSERT_TRUE(New.bCompactSplat);
        for (uint32 t = 0; t < Old.TriangleCount; ++t)
        {
            const uint32 OldPacked = Before.MeshletTriangles[Old.TriangleOffset + t];
            const uint32 NewPacked = After.MeshletTriangles[New.TriangleOffset + t];
            FVector3 A[3];
            FVector3 B[3];
            for (uint32 c = 0; c < 3u; ++c)
            {
                A[c] = GetMeshletVertexPosition(Before, Old, (OldPacked >> (c * 8u)) & 0xFFu, false);
                B[c] = GetMeshletVertexPosition(After, New, (NewPacked >> (c * 8u)) & 0xFFu, false);
            }
            const FVector3 OldCenter = (A[0] + A[1] + A[2]) / 3.0f;
            const FVector3 NewCenter = (B[0] + B[1] + B[2]) / 3.0f;
            EXPECT_LE(Math::Length(NewCenter - OldCenter), GridStep * 4.0f + 1e-4f);

            const float OldReach = Math::Length(A[0] - OldCenter);
            const float NewReach = Math::Length(B[0] - NewCenter);
            EXPECT_GE(NewReach, OldReach * 0.999f - GridStep * 4.0f);
            EXPECT_LE(NewReach, OldReach * 1.02f + GridStep * 4.0f);

            const FVector3 OldFace = Math::Normalize(Math::Cross(A[1] - A[0], A[2] - A[0]));
            const FVector3 NewFace = Math::Normalize(Math::Cross(B[1] - B[0], B[2] - B[0]));
            EXPECT_GT(Math::Dot(OldFace, NewFace), 0.99f);
        }
    }
}

#endif
