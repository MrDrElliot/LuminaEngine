#pragma once

#include "SceneRenderTypes.h"

#include <cmath>

namespace Lumina::SunShadow
{
    // Half the side of a virtual shadow level, which doubles from the first level's each level.
    inline float VirtualShadowLevelRadius(int32 Level)
    {
        return VSM_FIRST_LEVEL_RADIUS * std::exp2((float)Level);
    }

    // What turns a distance into its level, which the shaders read back as ceil(log2(Distance * Scale)).
    inline float VirtualShadowFootprintScale(float PixelWorldPerMeter, float ResolutionBias)
    {
        const float FirstTexel = VSM_FIRST_LEVEL_RADIUS * 2.0f / (float)VSM_VIRTUAL_SIZE;
        return PixelWorldPerMeter * std::exp2(ResolutionBias) / FirstTexel;
    }

    inline int32 VirtualShadowLevelForDistance(float Distance, float FootprintScale)
    {
        return (int32)std::ceil(std::log2(Math::Max(Distance * FootprintScale, 1e-8f)));
    }

    struct FVirtualShadowLevelBox
    {
        FMatrix4    ViewProjection = FMatrix4(1.0f);
        // The global page under the level's top left virtual page.
        FIntVector2 PageOrigin = FIntVector2(0);
        float       TexelWorld = 0.0f;
        float       DepthRange = 0.0f;
        // Which depth window the level sits in, since its cached depths only hold while this stays put.
        int64       DepthKey = 0;
    };

    // Centered on the camera snapped to whole pages in the light's plane, so a page keeps its texels while the level slides.
    inline FVirtualShadowLevelBox MakeVirtualShadowLevel(const FVector3& Camera, int32 Level, float MinHalfDepth, const FVector3& LightDir, const FVector3& Up)
    {
        FVirtualShadowLevelBox Box;
        const float Radius    = VirtualShadowLevelRadius(Level);
        Box.TexelWorld        = Radius * 2.0f / (float)VSM_VIRTUAL_SIZE;
        const float PageWorld = Box.TexelWorld * (float)VSM_PAGE_SIZE;
        const float HalfDepth = Math::Max(Radius, MinHalfDepth);
        const float DepthStep = HalfDepth * 0.25f;
        Box.DepthRange        = HalfDepth * 2.0f;

        const FMatrix4 LightRotation = Math::LookAt(LightDir, FVector3(0.0f), Up);
        const FVector4 CameraLS      = LightRotation * FVector4(Camera, 1.0f);
        const int64 PageX = (int64)std::floor(CameraLS.x / PageWorld + 0.5f);
        const int64 PageY = (int64)std::floor(CameraLS.y / PageWorld + 0.5f);
        Box.DepthKey      = (int64)std::floor(CameraLS.z / DepthStep + 0.5f);
        const FVector4 SnappedLS((float)PageX * PageWorld, (float)PageY * PageWorld, (float)Box.DepthKey * DepthStep, 1.0f);
        const FVector3 Snapped = FVector3(Math::Inverse(LightRotation) * SnappedLS);

        const FMatrix4 LightView = Math::LookAt(Snapped + LightDir * HalfDepth, Snapped, Up);
        FMatrix4 Projection = Math::Ortho(-Radius, Radius, -Radius, Radius, 0.0f, Box.DepthRange);
        Projection[1][1] *= -1.0f;
        Box.ViewProjection = Projection * LightView;

        // The projection flips y, so global page rows run against the light's up axis.
        const int64 HalfPages = (int64)VSM_LEVEL_PAGES / 2;
        Box.PageOrigin = FIntVector2((int32)(PageX - HalfPages), (int32)(-PageY - HalfPages));
        return Box;
    }
}
