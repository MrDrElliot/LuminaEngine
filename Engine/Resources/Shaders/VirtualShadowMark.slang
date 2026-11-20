#include "Includes/VirtualShadow.slang"

// Every opaque pixel asks for the pages its shadow filter will read, in the level its distance picks.

struct FPushConstants
{
    GPUSpan<FVirtualShadowPage> Pages;
    FTexture2D                  Depth;
    uint                        ScreenW;
    uint                        ScreenH;
    uint                        Frame;
    // Texels past the filter's own reach a request covers, for the normal offset the lighting adds.
    float                       MarginTexels;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

void RequestPages(uint Level, float3 WorldPos)
{
    const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);
    float2 Texel;
    float  ShadowDepth;
    if (!ProjectToVirtualShadow(Lv, WorldPos, Texel, ShadowDepth))
    {
        return;
    }

    const float Margin = Pass().MarginTexels;
    const int2  Lo = clamp(int2(floor(Texel - Margin)) >> int(VSM_PAGE_SHIFT), int2(0, 0), int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
    const int2  Hi = clamp(int2(floor(Texel + Margin)) >> int(VSM_PAGE_SHIFT), int2(0, 0), int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
    for (int y = Lo.y; y <= Hi.y; ++y)
    {
        for (int x = Lo.x; x <= Hi.x; ++x)
        {
            const uint Index = VirtualShadowPageIndex(Level, Lv.PageOrigin + int2(x, y));
            // Identical stores from every pixel of the page, so no atomic is needed, and a wave writes each page once.
            if (WaveIsFirstLane() || !WaveActiveAllEqual(Index))
            {
                Pass().Pages.At(Index).LastRequested = Pass().Frame;
            }
        }
    }
}

[shader("compute")]
[numthreads(VSM_MARK_GROUP_SIZE, VSM_MARK_GROUP_SIZE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().ScreenW || DTid.y >= Pass().ScreenH)
    {
        return;
    }

    // Reverse Z, so the sky is the zero the clear left.
    const float Depth = Pass().Depth.Load(int2(DTid.xy)).r;
    if (Depth <= 0.0)
    {
        return;
    }

    const float2 UV       = (float2(DTid.xy) + 0.5) / float2(Pass().ScreenW, Pass().ScreenH);
    const float3 WorldPos = ReconstructWorldPos(UV, Depth);
    const float  Distance = length(WorldPos - GetCameraPosition());
    const int    Wanted   = VirtualShadowLevelForDistance(Distance);
    if (Wanted > (int)LightData().SunShadow.LastLevel)
    {
        return;
    }

    const uint Level = ClampVirtualShadowLevel(Wanted);
    RequestPages(Level, WorldPos);

    // The blend band samples the next level as well, so it has to be resident there.
    if (Level == (uint)Wanted && Level < LightData().SunShadow.LastLevel && VirtualShadowLevelBlend(Distance) > 0.0)
    {
        RequestPages(Level + 1u, WorldPos);
    }
}
