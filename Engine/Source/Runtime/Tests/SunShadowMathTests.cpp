#include <gtest/gtest.h>

#include "World/Scene/RenderScene/SunShadowMath.h"

#include <random>

using namespace Lumina;

namespace
{
    FVector2 VirtualGlobalTexel(const SunShadow::FVirtualShadowLevelBox& Box, const FVector3& Point)
    {
        const FVector4 Ndc = Box.ViewProjection * FVector4(Point, 1.0f);
        const FVector2 Texel = (FVector2(Ndc.x, Ndc.y) * 0.5f + 0.5f) * (float)VSM_VIRTUAL_SIZE;
        return Texel + FVector2(Box.PageOrigin) * (float)VSM_PAGE_SIZE;
    }
}

TEST(SunShadowMath, VirtualLevelKeepsItsTexelsWhileTheCameraMoves)
{
    const FVector3 LightDir = Math::Normalize(FVector3(0.3f, 0.8f, 0.5f));
    const FVector3 Up(0.0f, 1.0f, 0.0f);
    const SunShadow::FVirtualShadowLevelBox A = SunShadow::MakeVirtualShadowLevel(FVector3(10.0f, 2.0f, -5.0f), 3, 100.0f, LightDir, Up);
    const SunShadow::FVirtualShadowLevelBox B = SunShadow::MakeVirtualShadowLevel(FVector3(37.3f, 5.0f, 12.1f), 3, 100.0f, LightDir, Up);
    EXPECT_NE(A.PageOrigin, B.PageOrigin);

    std::mt19937 Rng(7);
    std::uniform_real_distribution<float> Offset(-10.0f, 10.0f);
    for (int i = 0; i < 64; ++i)
    {
        const FVector3 Point(20.0f + Offset(Rng), 3.0f + Offset(Rng), 4.0f + Offset(Rng));
        const FVector2 FromA = VirtualGlobalTexel(A, Point);
        const FVector2 FromB = VirtualGlobalTexel(B, Point);
        EXPECT_NEAR(FromA.x, FromB.x, 0.05f);
        EXPECT_NEAR(FromA.y, FromB.y, 0.05f);
    }
}

TEST(SunShadowMath, VirtualLevelCentersTheCameraWithinAPage)
{
    const FVector3 LightDir = Math::Normalize(FVector3(-0.4f, 0.7f, 0.2f));
    const FVector3 Camera(-1500.0f, 40.0f, -1200.0f);
    const SunShadow::FVirtualShadowLevelBox Box = SunShadow::MakeVirtualShadowLevel(Camera, 6, 100.0f, LightDir, FVector3(0.0f, 1.0f, 0.0f));
    const FVector4 Ndc = Box.ViewProjection * FVector4(Camera, 1.0f);
    const FVector2 Texel = (FVector2(Ndc.x, Ndc.y) * 0.5f + 0.5f) * (float)VSM_VIRTUAL_SIZE;
    EXPECT_NEAR(Texel.x, (float)VSM_VIRTUAL_SIZE * 0.5f, (float)VSM_PAGE_SIZE);
    EXPECT_NEAR(Texel.y, (float)VSM_VIRTUAL_SIZE * 0.5f, (float)VSM_PAGE_SIZE);
    EXPECT_GT(Ndc.z, 0.0f);
    EXPECT_LT(Ndc.z, 1.0f);
}

TEST(SunShadowMath, VirtualLevelTexelMatchesThePixelFootprint)
{
    const float PixelWorldPerMeter = 0.0007f;
    const float Scale = SunShadow::VirtualShadowFootprintScale(PixelWorldPerMeter, 0.0f);
    for (const float Distance : { 0.5f, 3.0f, 17.0f, 120.0f, 480.0f })
    {
        const int32 Level     = SunShadow::VirtualShadowLevelForDistance(Distance, Scale);
        const float Footprint = Distance * PixelWorldPerMeter;
        const float Texel     = SunShadow::VirtualShadowLevelRadius(Level) * 2.0f / (float)VSM_VIRTUAL_SIZE;
        EXPECT_GE(Texel, Footprint * 0.999f);
        EXPECT_LT(Texel * 0.5f, Footprint);
    }
}
