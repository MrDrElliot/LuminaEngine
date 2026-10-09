#include <gtest/gtest.h>

#include "World/Scene/RenderScene/SceneMeshes.h"

using namespace Lumina;

// A viewer facing a cube face from outside sees U run to their right and V run down, so a texture reads upright.
TEST(PrimitiveMeshes, CubeFacesShowTexturesUprightAndUnmirrored)
{
    TVector<FSourceVertex> Vertices;
    TVector<uint32> Indices;
    PrimitiveMeshes::GenerateCube(Vertices, Indices);
    ASSERT_EQ(Vertices.size(), 24u);

    for (size_t Face = 0; Face < 6; ++Face)
    {
        FVector3 Center(0.0f);
        for (size_t Corner = 0; Corner < 4; ++Corner)
        {
            Center += Vertices[Face * 4 + Corner].Position * 0.25f;
        }
        const FVector3 Normal = Math::Normalize(Center);
        const FVector3 Forward = -Normal;

        // Left-handed with Y up, so the right of a viewer looking along Forward is Up cross Forward.
        const FVector3 Up = Math::Abs(Normal.y) > 0.5f ? FVector3(0.0f, 0.0f, -1.0f) : FVector3(0.0f, 1.0f, 0.0f);
        const FVector3 Right = Math::Cross(Up, Forward);

        for (size_t Corner = 0; Corner < 4; ++Corner)
        {
            const FSourceVertex& Vertex = Vertices[Face * 4 + Corner];
            const FVector2 UV = Math::UnpackHalf2x16(Vertex.UV);
            const FVector3 Offset = Vertex.Position - Center;
            EXPECT_NEAR(UV.x, 0.5f + Math::Dot(Offset, Right), 1e-3f) << "face " << Face << " corner " << Corner;
            EXPECT_NEAR(UV.y, 0.5f - Math::Dot(Offset, Up), 1e-3f) << "face " << Face << " corner " << Corner;
        }
    }
}
