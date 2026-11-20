#include "gtest/gtest.h"

// Builds meshlets through the importer, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Assets/AssetTypes/Foliage/GrassPatch.h"
#include "Core/Math/Math.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/Vertex.h"

using namespace Lumina;

namespace
{
    // A blade as one upright quad, 0.1 wide and 0.5 tall, standing on the origin.
    FMeshResource MakeBlade()
    {
        FMeshResource Blade;
        Blade.ResizeVertices(4);
        const FVector3 Corners[4] = { { -0.05f, 0.0f, 0.0f }, { 0.05f, 0.0f, 0.0f }, { -0.05f, 0.5f, 0.0f }, { 0.05f, 0.5f, 0.0f } };
        for (int32 v = 0; v < 4; ++v)
        {
            Blade.Positions[v] = Corners[v];
            Blade.Normals[v]   = PackNormal(FVector3(0.0f, 0.0f, 1.0f));
            Blade.Tangents[v]  = PackTangent(FVector3(1.0f, 0.0f, 0.0f), 1.0f);
            Blade.UVs[v]       = Math::PackHalf2x16(FVector2((float)(v & 1), (float)(v >> 1)));
            Blade.UVs1[v]      = Blade.UVs[v];
            Blade.Colors[v]    = 0xFFFFFFFFu;
        }
        Blade.Indices = { 0u, 1u, 2u, 2u, 1u, 3u };
        FGeometrySurface& Surface = Blade.GeometrySurfaces.EmplaceGetRef();
        Surface.StartIndex    = 0;
        Surface.IndexCount    = 6;
        Surface.MaterialIndex = 0;
        return Blade;
    }
}

TEST(GrassPatch, HoldsTheDensityOverItsSquareAndStaysInsideIt)
{
    const FMeshResource Blade = MakeBlade();
    GrassPatch::FParams Params;
    Params.Density = 30.0f;

    FMeshResource Patch;
    ASSERT_TRUE(GrassPatch::Build(Blade, Params, Patch));

    const float Size   = GrassPatch::GetPatchSize(Params.Density);
    const int32 Blades = Patch.GetNumVertices() / Blade.GetNumVertices();
    EXPECT_EQ(Blades, (int32)Math::Round(Params.Density * Size * Size));
    ASSERT_EQ(Patch.GeometrySurfaces.Num(), 1);
    EXPECT_EQ(Patch.GeometrySurfaces[0].IndexCount, (uint32)(Blade.Indices.Num() * Blades));
    EXPECT_FALSE(Patch.MeshletData.IsEmpty());

    // Each blade root lies in the square, so its corners reach no further than the blade's own half width past the edge.
    const float Reach = Size * 0.5f + 0.05f * Params.ScaleMax + 1e-3f;
    for (const FVector3& P : Patch.Positions)
    {
        EXPECT_LE(Math::Abs(P.x), Reach);
        EXPECT_LE(Math::Abs(P.z), Reach);
    }
}

TEST(GrassPatch, SparseTypesStopAtTheLargestPatch)
{
    EXPECT_LE(GrassPatch::GetPatchSize(0.01f), 8.0f);
    EXPECT_GE(GrassPatch::GetPatchSize(1000.0f), 1.0f);
}

#endif
