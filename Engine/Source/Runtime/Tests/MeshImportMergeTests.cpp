#include "gtest/gtest.h"

#include "Renderer/MeshData.h"
#include "Tools/Import/ImportHelpers.h"

namespace LuminaMeshImportMergeTests
{
    using namespace Lumina;
    using namespace Lumina::Import::Mesh;

    TUniquePtr<FMeshResource> MakeTrianglePiece(float OffsetX, int16 SourceMaterial)
    {
        TUniquePtr<FMeshResource> Piece = MakeUnique<FMeshResource>();
        Piece->ResizeVertices(3);
        Piece->Positions[0] = FVector3(OffsetX, 0.0f, 0.0f);
        Piece->Positions[1] = FVector3(OffsetX + 1.0f, 0.0f, 0.0f);
        Piece->Positions[2] = FVector3(OffsetX, 1.0f, 0.0f);
        for (int32 i = 0; i < 3; ++i)
        {
            Piece->Normals[i] = PackNormal(FVector3(0.0f, 0.0f, 1.0f));
        }
        Piece->Indices = { 0u, 1u, 2u };

        FGeometrySurface& Surface = Piece->GeometrySurfaces.EmplaceGetRef();
        Surface.IndexCount    = 3;
        Surface.MaterialIndex = SourceMaterial;
        return Piece;
    }

    TEST(MeshImportMerge, PiecesSharingAMaterialShareOneSlot)
    {
        FMeshImportData Data;
        Data.Resources.Add(MakeTrianglePiece(0.0f, 0));
        Data.Resources.Add(MakeTrianglePiece(2.0f, 0));
        Data.Resources.Add(MakeTrianglePiece(4.0f, 1));
        Data.Resources.Add(MakeTrianglePiece(6.0f, 0));

        FMeshImportOptions Options;
        Options.bMergeMeshes = true;
        Options.bOptimize    = false;
        FinalizeMeshImportData(Data, Options);

        ASSERT_EQ(Data.Resources.Num(), 1);
        ASSERT_EQ(Data.MergedMaterialSlotToSource.Num(), 2);
        EXPECT_EQ(Data.MergedMaterialSlotToSource[0], 0);
        EXPECT_EQ(Data.MergedMaterialSlotToSource[1], 1);

        const FMeshResource& Merged = *Data.Resources[0];
        ASSERT_EQ(Merged.GeometrySurfaces.Num(), 2);
        EXPECT_EQ(Merged.GeometrySurfaces[0].MaterialIndex, 0);
        EXPECT_EQ(Merged.GeometrySurfaces[0].IndexCount, 9u);
        EXPECT_EQ(Merged.GeometrySurfaces[1].MaterialIndex, 1);
        EXPECT_EQ(Merged.GeometrySurfaces[1].IndexCount, 3u);
    }
}
