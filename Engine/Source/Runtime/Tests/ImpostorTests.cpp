#include <gtest/gtest.h>
#include "Renderer/MeshImpostor.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "encoder/basisu_enc.h"
#include "transcoder/basisu_transcoder_internal.h"

using namespace Lumina;

namespace
{
    // A smooth gradient in color with a disc of coverage per view, the shape a baked atlas has.
    FMeshImpostor MakeAtlas()
    {
        FMeshImpostor Out;
        Out.Frames    = 2;
        Out.FrameSize = 16;
        Out.Radius    = 1.0f;
        Out.BaseColor.SetNumZeroed((int32)Out.GetByteCount());
        Out.NormalDepth.SetNumZeroed((int32)Out.GetByteCount());
        for (uint32 Mip = 0; Mip < Out.GetMipCount(); ++Mip)
        {
            const uint32 Side = Out.GetAtlasSize() >> Mip;
            uint8* Color = Out.BaseColor.GetData() + Out.GetMipOffset(Mip);
            uint8* Data  = Out.NormalDepth.GetData() + Out.GetMipOffset(Mip);
            for (uint32 Y = 0; Y < Side; ++Y)
            {
                for (uint32 X = 0; X < Side; ++X)
                {
                    uint8* C = Color + (Y * Side + X) * 4u;
                    uint8* D = Data + (Y * Side + X) * 4u;
                    C[0] = (uint8)(X * 255u / Side);
                    C[1] = (uint8)(Y * 255u / Side);
                    C[2] = 96;
                    const uint32 ViewSide = Out.FrameSize >> Mip;
                    const float  Dx = ((float)(X % ViewSide) + 0.5f) / (float)ViewSide - 0.5f;
                    const float  Dy = ((float)(Y % ViewSide) + 0.5f) / (float)ViewSide - 0.5f;
                    C[3] = (uint8)(Math::Clamp((0.4f - Math::Sqrt(Dx * Dx + Dy * Dy)) * 8.0f, 0.0f, 1.0f) * 255.0f);
                    D[0] = (uint8)(255u - X * 255u / Side);
                    D[1] = 128;
                    D[2] = (uint8)(Y * 255u / Side);
                    D[3] = 200;
                }
            }
        }
        return Out;
    }

    int32 MaxBlockError(const FMeshImpostor& Source, const FMeshImpostor& Compressed, const TVector<uint8>& Rgba, const TVector<uint8>& Blocks, uint32 Mip, bool bCoverageInAlpha)
    {
        const uint32 Side   = Source.GetAtlasSize() >> Mip;
        const uint8* Rows   = Rgba.GetData() + Source.GetMipOffset(Mip);
        const uint8* Packed = Blocks.GetData() + Compressed.GetMipOffset(Mip);
        int32 Worst = 0;
        for (uint32 By = 0; By < Side / 4u; ++By)
        {
            for (uint32 Bx = 0; Bx < Side / 4u; ++Bx)
            {
                basist::color_rgba Pixels[16];
                EXPECT_TRUE(basist::bc7u::unpack_bc7(Packed + ((uint64)By * (Side / 4u) + Bx) * 16u, Pixels));
                for (uint32 i = 0; i < 16u; ++i)
                {
                    const uint8* Expected = Rows + ((uint64)(By * 4u + i / 4u) * Side + Bx * 4u + i % 4u) * 4u;
                    // Color under no coverage never shows, so only a visible texel's color counts.
                    const uint32 FirstChannel = bCoverageInAlpha && Expected[3] < 64u ? 3u : 0u;
                    for (uint32 c = FirstChannel; c < 4u; ++c)
                    {
                        Worst = Math::Max(Worst, Math::Abs((int32)Pixels[i].m_comps[c] - (int32)Expected[c]));
                    }
                }
            }
        }
        return Worst;
    }
}

TEST(MeshImpostor, CompressKeepsEveryMipBlockForBlock)
{
    basisu::basisu_encoder_init();
    const FMeshImpostor Source = MakeAtlas();
    FMeshImpostor Compressed = Source;
    Impostor::Compress(Compressed);

    ASSERT_TRUE(Compressed.bCompressed);
    ASSERT_TRUE(Compressed.IsValid());
    EXPECT_EQ(Compressed.GetMipCount(), 3u);
    for (uint32 Mip = 0; Mip < Compressed.GetMipCount(); ++Mip)
    {
        EXPECT_LE(MaxBlockError(Source, Compressed, Source.BaseColor, Compressed.BaseColor, Mip, true), 40) << "base color mip " << Mip;
        EXPECT_LE(MaxBlockError(Source, Compressed, Source.NormalDepth, Compressed.NormalDepth, Mip, false), 24) << "normal depth mip " << Mip;
    }
}

TEST(MeshImpostor, RotationPacksWithinAFewThousandths)
{
    const FQuat Rotations[] = { FQuat(1.0f, 0.0f, 0.0f, 0.0f), Math::Normalize(FQuat(0.3f, -0.5f, 0.7f, 0.2f)), Math::Normalize(FQuat(-0.9f, 0.1f, 0.05f, -0.4f)) };
    for (const FQuat& Q : Rotations)
    {
        const uint32 Packed = PackImpostorRotation(Q);
        const uint32 Largest = Packed >> 30;
        float Small[3];
        for (uint32 i = 0; i < 3; ++i)
        {
            Small[i] = ((float)((Packed >> (20u - i * 10u)) & 1023u) / 1023.0f - 0.5f) * 1.41421356f;
        }
        const float Big = Math::Sqrt(Math::Max(1.0f - Small[0] * Small[0] - Small[1] * Small[1] - Small[2] * Small[2], 0.0f));
        float Decoded[4];
        uint32 Next = 0;
        for (uint32 i = 0; i < 4; ++i)
        {
            Decoded[i] = i == Largest ? Big : Small[Next++];
        }
        const float Original[4] = { Q.x, Q.y, Q.z, Q.w };
        const float Sign = Original[Largest] < 0.0f ? -1.0f : 1.0f;
        for (uint32 i = 0; i < 4; ++i)
        {
            EXPECT_NEAR(Decoded[i], Original[i] * Sign, 0.003f);
        }
    }
}

TEST(MeshImpostor, PointModelMatchesTheFoliageTransform)
{
    const FQuat Rotations[] = { Math::Normalize(FQuat(0.3f, -0.5f, 0.7f, 0.2f)), Math::Normalize(FQuat(-0.9f, 0.1f, 0.05f, -0.4f)), Math::Normalize(FQuat(0.2f, 0.1f, 0.95f, 0.0f)) };
    for (const FQuat& Q : Rotations)
    {
        SFoliageInstance Instance;
        Instance.Position = FVector3(12.0f, -3.0f, 40.0f);
        Instance.SetRotationQuat(Q);
        Instance.Scale = FVector3(1.7f);
        const FMatrix4 Expected = Instance.GetMatrix();

        // ImpostorPointModel in Common.slang, rows of the model matrix.
        const float x = Q.x, y = Q.y, z = Q.z, w = Q.w, S = 1.7f;
        const float Rows[3][4] = {
            { (1.0f - 2.0f * (y * y + z * z)) * S, 2.0f * (x * y - w * z) * S, 2.0f * (x * z + w * y) * S, 12.0f },
            { 2.0f * (x * y + w * z) * S, (1.0f - 2.0f * (x * x + z * z)) * S, 2.0f * (y * z - w * x) * S, -3.0f },
            { 2.0f * (x * z - w * y) * S, 2.0f * (y * z + w * x) * S, (1.0f - 2.0f * (x * x + y * y)) * S, 40.0f } };
        for (uint32 r = 0; r < 3; ++r)
        {
            for (uint32 c = 0; c < 4; ++c)
            {
                EXPECT_NEAR(Rows[r][c], Expected[c][r], 1.0e-4f) << "row " << r << " column " << c;
            }
        }
    }
}
