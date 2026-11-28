#include <gtest/gtest.h>
#include "Containers/HashTable.h"
#include "World/Scene/RenderScene/VirtualTextureTypes.h"

using namespace Lumina;

namespace
{
    bool InWindow(const FIntVector2& Origin, int32 Resolution, int32 X, int32 Y)
    {
        return X >= Origin.x && Y >= Origin.y && X < Origin.x + Resolution && Y < Origin.y + Resolution;
    }

    // Every texel the new window holds that the old one did not is covered, and nothing outside the new window is.
    void ExpectExposedCoverage(const FIntVector2& Old, const FIntVector2& New, int32 Resolution)
    {
        TVector<FIntVector4> Rects;
        VirtualTextureMath::ExposedRects(Old, New, Resolution, Rects);
        for (int32 Y = New.y; Y < New.y + Resolution; ++Y)
        {
            for (int32 X = New.x; X < New.x + Resolution; ++X)
            {
                bool bCovered = false;
                for (const FIntVector4& R : Rects)
                {
                    bCovered = bCovered || (X >= R.x && Y >= R.y && X < R.x + R.z && Y < R.y + R.w);
                }
                EXPECT_EQ(bCovered, !InWindow(Old, Resolution, X, Y)) << X << "," << Y << " moving " << Old.x << "," << Old.y << " to " << New.x << "," << New.y;
            }
        }
        for (const FIntVector4& R : Rects)
        {
            EXPECT_TRUE(InWindow(New, Resolution, R.x, R.y) && InWindow(New, Resolution, R.x + R.z - 1, R.y + R.w - 1));
        }
    }
}

TEST(VirtualTexture, WindowOriginSnapsToWholeTexelsAroundTheCenter)
{
    EXPECT_EQ(VirtualTextureMath::WindowOrigin(FVector2(0.0f), 0.25f, 64), FIntVector2(-32, -32));
    EXPECT_EQ(VirtualTextureMath::WindowOrigin(FVector2(0.2f, -0.2f), 0.25f, 64), FIntVector2(-32, -33));
    // A small move inside one texel leaves the window where it was, so nothing needs redrawing.
    EXPECT_EQ(VirtualTextureMath::WindowOrigin(FVector2(10.01f, 5.0f), 0.5f, 128), VirtualTextureMath::WindowOrigin(FVector2(10.2f, 5.1f), 0.5f, 128));
}

TEST(VirtualTexture, ExposedRectsCoverExactlyWhatTheMoveUncovers)
{
    constexpr int32 Res = 16;
    const FIntVector2 Old(3, -5);
    for (const FIntVector2& Delta : { FIntVector2(1, 0), FIntVector2(-3, 0), FIntVector2(0, 5), FIntVector2(0, -1),
                                      FIntVector2(4, -2), FIntVector2(-7, 9), FIntVector2(15, 15), FIntVector2(16, 0), FIntVector2(-40, 3) })
    {
        ExpectExposedCoverage(Old, Old + Delta, Res);
    }

    TVector<FIntVector4> None;
    VirtualTextureMath::ExposedRects(Old, Old, Res, None);
    EXPECT_TRUE(None.IsEmpty());
}

TEST(VirtualTexture, WrappedTexelsOfOneWindowAreAllDistinct)
{
    constexpr int32 Res = 32;
    for (const FIntVector2& Origin : { FIntVector2(0, 0), FIntVector2(-45, 7), FIntVector2(1000003, -999999) })
    {
        THashSet<int32> Seen;
        for (int32 Y = Origin.y; Y < Origin.y + Res; ++Y)
        {
            for (int32 X = Origin.x; X < Origin.x + Res; ++X)
            {
                const int32 WX = VirtualTextureMath::Wrap(X, Res);
                const int32 WY = VirtualTextureMath::Wrap(Y, Res);
                ASSERT_GE(WX, 0);
                ASSERT_LT(WX, Res);
                ASSERT_GE(WY, 0);
                ASSERT_LT(WY, Res);
                Seen.Add(WY * Res + WX);
            }
        }
        EXPECT_EQ(Seen.Num(), Res * Res);
    }
}
