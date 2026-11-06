#include "gtest/gtest.h"

#include <cfloat>

#include "Core/Math/Math.h"
#include "World/Entity/Components/BlockoutComponent.h"
#include "World/Subsystems/BlockoutMeshBuilder.h"

// Covers the blockout primitive generator alone, so no world, ECS or renderer is involved.

using namespace Lumina;

namespace
{
    SBlockoutComponent MakeShape(EBlockoutShape Shape)
    {
        SBlockoutComponent Out;
        Out.Shape = Shape;
        Out.Size = FVector3(2.0f, 3.0f, 4.0f);
        Out.Thickness = 0.25f;
        Out.RadialSegments = 12;
        Out.RingSegments = 6;
        Out.Steps = 5;
        return Out;
    }

    const EBlockoutShape AllShapes[] =
    {
        EBlockoutShape::Box,
        EBlockoutShape::Plane,
        EBlockoutShape::Ramp,
        EBlockoutShape::Stairs,
        EBlockoutShape::Cylinder,
        EBlockoutShape::Cone,
        EBlockoutShape::Sphere,
        EBlockoutShape::Capsule,
        EBlockoutShape::Torus,
        EBlockoutShape::Arch,
        EBlockoutShape::Pipe,
    };

    // Positive only when the winding agrees with the right-hand rule the renderer culls against.
    double SignedVolume(const FBlockoutMeshData& Mesh)
    {
        double Volume = 0.0;
        for (int32 Index = 0; Index + 2 < Mesh.Indices.Num(); Index += 3)
        {
            const FVector3& A = Mesh.Positions[Mesh.Indices[Index]];
            const FVector3& B = Mesh.Positions[Mesh.Indices[Index + 1]];
            const FVector3& C = Mesh.Positions[Mesh.Indices[Index + 2]];
            Volume += (double)Math::Dot(A, Math::Cross(B, C));
        }
        return Volume / 6.0;
    }

    // The fraction of triangles whose winding normal disagrees with the normals the shader will shade with.
    float DisagreeingNormalFraction(const FBlockoutMeshData& Mesh)
    {
        int32 Disagreeing = 0;
        int32 Total = 0;

        for (int32 Index = 0; Index + 2 < Mesh.Indices.Num(); Index += 3)
        {
            const uint32 I0 = Mesh.Indices[Index];
            const uint32 I1 = Mesh.Indices[Index + 1];
            const uint32 I2 = Mesh.Indices[Index + 2];

            const FVector3 Edge0 = Mesh.Positions[I1] - Mesh.Positions[I0];
            const FVector3 Edge1 = Mesh.Positions[I2] - Mesh.Positions[I0];
            const FVector3 Cross = Math::Cross(Edge0, Edge1);
            if (Math::Length(Cross) < 1e-9f)
            {
                continue;
            }

            const FVector3 WindingNormal = Math::Normalize(Cross);
            const FVector3 Shaded = Mesh.Normals[I0] + Mesh.Normals[I1] + Mesh.Normals[I2];

            ++Total;
            if (Math::Dot(WindingNormal, Shaded) <= 0.0f)
            {
                ++Disagreeing;
            }
        }

        return Total == 0 ? 1.0f : float(Disagreeing) / float(Total);
    }
}

TEST(BlockoutMesh, EveryShapeGeneratesGeometry)
{
    for (EBlockoutShape Shape : AllShapes)
    {
        FBlockoutMeshData Mesh;
        BlockoutMesh::Build(MakeShape(Shape), Mesh);

        EXPECT_FALSE(Mesh.IsEmpty()) << BlockoutMesh::GetShapeName(Shape);
        EXPECT_EQ(Mesh.Positions.Num(), Mesh.Normals.Num()) << BlockoutMesh::GetShapeName(Shape);
        EXPECT_EQ(Mesh.Positions.Num(), Mesh.UVs.Num()) << BlockoutMesh::GetShapeName(Shape);
        EXPECT_EQ(Mesh.Indices.Num() % 3, 0) << BlockoutMesh::GetShapeName(Shape);

        for (uint32 Index : Mesh.Indices)
        {
            ASSERT_LT(Index, static_cast<uint32>(Mesh.Positions.Num())) << BlockoutMesh::GetShapeName(Shape);
        }
    }
}

TEST(BlockoutMesh, WindingMatchesShadingNormals)
{
    for (EBlockoutShape Shape : AllShapes)
    {
        FBlockoutMeshData Mesh;
        BlockoutMesh::Build(MakeShape(Shape), Mesh);

        // Coarse radial tessellation makes a few seam triangles borderline, so this is a majority test.
        EXPECT_LT(DisagreeingNormalFraction(Mesh), 0.02f) << BlockoutMesh::GetShapeName(Shape);
    }
}

TEST(BlockoutMesh, ClosedShapesWindOutward)
{
    const EBlockoutShape ClosedShapes[] =
    {
        EBlockoutShape::Box,
        EBlockoutShape::Ramp,
        EBlockoutShape::Stairs,
        EBlockoutShape::Cylinder,
        EBlockoutShape::Cone,
        EBlockoutShape::Sphere,
        EBlockoutShape::Capsule,
        EBlockoutShape::Torus,
        EBlockoutShape::Arch,
        EBlockoutShape::Pipe,
    };

    for (EBlockoutShape Shape : ClosedShapes)
    {
        FBlockoutMeshData Mesh;
        BlockoutMesh::Build(MakeShape(Shape), Mesh);

        EXPECT_GT(SignedVolume(Mesh), 0.0) << BlockoutMesh::GetShapeName(Shape);
    }
}

TEST(BlockoutMesh, BoxVolumeMatchesItsSize)
{
    SBlockoutComponent Shape = MakeShape(EBlockoutShape::Box);
    FBlockoutMeshData Mesh;
    BlockoutMesh::Build(Shape, Mesh);

    EXPECT_NEAR(SignedVolume(Mesh), 2.0 * 3.0 * 4.0, 1e-3);
}

TEST(BlockoutMesh, GeneratedPositionsStayInsideReportedBounds)
{
    for (EBlockoutShape Shape : AllShapes)
    {
        for (EBlockoutPivot Pivot : { EBlockoutPivot::Base, EBlockoutPivot::Center })
        {
            SBlockoutComponent Params = MakeShape(Shape);
            Params.Pivot = Pivot;

            FBlockoutMeshData Mesh;
            BlockoutMesh::Build(Params, Mesh);

            FVector3 Min, Max;
            BlockoutMesh::GetLocalBounds(Params, Min, Max);

            for (const FVector3& Position : Mesh.Positions)
            {
                EXPECT_GE(Position.x, Min.x - 1e-3f) << BlockoutMesh::GetShapeName(Shape);
                EXPECT_GE(Position.y, Min.y - 1e-3f) << BlockoutMesh::GetShapeName(Shape);
                EXPECT_GE(Position.z, Min.z - 1e-3f) << BlockoutMesh::GetShapeName(Shape);
                EXPECT_LE(Position.x, Max.x + 1e-3f) << BlockoutMesh::GetShapeName(Shape);
                EXPECT_LE(Position.y, Max.y + 1e-3f) << BlockoutMesh::GetShapeName(Shape);
                EXPECT_LE(Position.z, Max.z + 1e-3f) << BlockoutMesh::GetShapeName(Shape);
            }
        }
    }
}

TEST(BlockoutMesh, BasePivotRestsOnTheOrigin)
{
    SBlockoutComponent Shape = MakeShape(EBlockoutShape::Cylinder);
    Shape.Pivot = EBlockoutPivot::Base;

    FBlockoutMeshData Mesh;
    BlockoutMesh::Build(Shape, Mesh);

    float LowestY = FLT_MAX;
    for (const FVector3& Position : Mesh.Positions)
    {
        LowestY = Math::Min(LowestY, Position.y);
    }

    EXPECT_NEAR(LowestY, 0.0f, 1e-4f);
}

TEST(BlockoutMesh, CenterPivotHalvesTheHeight)
{
    SBlockoutComponent Shape = MakeShape(EBlockoutShape::Box);
    Shape.Pivot = EBlockoutPivot::Center;

    FBlockoutMeshData Mesh;
    BlockoutMesh::Build(Shape, Mesh);

    float LowestY = FLT_MAX;
    float HighestY = -FLT_MAX;
    for (const FVector3& Position : Mesh.Positions)
    {
        LowestY  = Math::Min(LowestY, Position.y);
        HighestY = Math::Max(HighestY, Position.y);
    }

    EXPECT_NEAR(LowestY, -Shape.Size.y * 0.5f, 1e-4f);
    EXPECT_NEAR(HighestY, Shape.Size.y * 0.5f, 1e-4f);
}

TEST(BlockoutMesh, ParameterHashTracksAuthoredFieldsOnly)
{
    SBlockoutComponent Shape = MakeShape(EBlockoutShape::Box);
    const uint32 Baseline = BlockoutMesh::HashParameters(Shape);

    SBlockoutComponent Same = Shape;
    Same.bBuilt = true;
    Same.BuiltHash = 0x12345678;
    Same.BuiltMaterial = &Shape;
    EXPECT_EQ(BlockoutMesh::HashParameters(Same), Baseline);

    SBlockoutComponent Resized = Shape;
    Resized.Size.y += 0.5f;
    EXPECT_NE(BlockoutMesh::HashParameters(Resized), Baseline);

    SBlockoutComponent Restepped = Shape;
    Restepped.Steps += 1;
    EXPECT_NE(BlockoutMesh::HashParameters(Restepped), Baseline);

    SBlockoutComponent Repivoted = Shape;
    Repivoted.Pivot = EBlockoutPivot::Center;
    EXPECT_NE(BlockoutMesh::HashParameters(Repivoted), Baseline);
}

TEST(BlockoutMesh, DegenerateSizeStillBuilds)
{
    SBlockoutComponent Shape = MakeShape(EBlockoutShape::Box);
    Shape.Size = FVector3(0.0f);
    Shape.Thickness = 0.0f;
    Shape.UVScale = 0.0f;
    Shape.RadialSegments = 0;
    Shape.RingSegments = 0;
    Shape.Steps = 0;

    FBlockoutMeshData Mesh;
    BlockoutMesh::Build(Shape, Mesh);

    EXPECT_FALSE(Mesh.IsEmpty());
    for (const FVector3& Position : Mesh.Positions)
    {
        EXPECT_TRUE(std::isfinite(Position.x));
        EXPECT_TRUE(std::isfinite(Position.y));
        EXPECT_TRUE(std::isfinite(Position.z));
    }
}
