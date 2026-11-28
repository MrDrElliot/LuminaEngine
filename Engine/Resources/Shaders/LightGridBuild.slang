#pragma once
#include "Includes/SceneGlobals.slang"

// The screen tiles a sphere's view-space box covers, with the box cut at the near plane so a light around the camera marks only what it reaches.
uint4 SphereTileRect(float3 ViewCenter, float Depth, float Radius, float3 ViewForward)
{
    const uint2 Grid      = max(Scene().GridSize.xy, uint2(1u, 1u));
    const float NearDepth = max(Depth - Radius, GetNearPlane() * 1.001);
    const float FarDepth  = max(Depth + Radius, NearDepth);
    // View space looks along one of its own axes, so a depth maps to a z by that axis's sign.
    const float AxisSign  = ViewForward.z < 0.0 ? -1.0 : 1.0;

    float2 MinUV = float2( 1.0e30,  1.0e30);
    float2 MaxUV = float2(-1.0e30, -1.0e30);
    [unroll]
    for (uint Corner = 0u; Corner < 8u; ++Corner)
    {
        const float3 Point = float3(ViewCenter.x + ((Corner & 1u) ? Radius : -Radius),
                                    ViewCenter.y + ((Corner & 2u) ? Radius : -Radius),
                                    AxisSign * ((Corner & 4u) ? FarDepth : NearDepth));
        const float4 Clip = ViewToClip(Point);
        const float2 UV   = Clip.xy / Clip.w * 0.5 + 0.5;
        MinUV = min(MinUV, UV);
        MaxUV = max(MaxUV, UV);
    }

    const float2 Tiles = float2(Grid);
    const float2 Lo    = clamp(floor(saturate(MinUV) * Tiles), float2(0.0, 0.0), Tiles - 1.0);
    const float2 Hi    = clamp(floor(saturate(MaxUV) * Tiles), float2(0.0, 0.0), Tiles - 1.0);
    return uint4((uint2)Lo, (uint2)Hi);
}

// One thread per sorted light, so a wave holds lights of nearly one depth and settles the depth bins they share with one atomic pair each.
[shader("compute")]
[numthreads(LIGHT_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    // Slot 0 is the sun, which no bin or tile lists.
    const uint Sorted = DispatchID.x + 1u;
    const bool bValid = Sorted < LightGridWord(LIGHT_GRID_NUM_LIGHTS) && ViewLights().Contains(Sorted);

    float4 PositionRadius = float4(0.0, 0.0, 0.0, 0.0);
    if (bValid)
    {
        PositionRadius = LightBoundingSphere(ViewLights().GetAligned(Sorted));
    }
    const float3 ViewForward = NormalWorldToView(GetCameraForward());
    const float3 ViewCenter  = WorldToView(PositionRadius.xyz);
    const float  Depth       = dot(ViewCenter, ViewForward);
    const uint   FirstBin    = bValid ? LightZBin(Depth - PositionRadius.w) : 0xFFFFFFFFu;
    const uint   LastBin     = bValid ? LightZBin(Depth + PositionRadius.w) : 0u;

    // A light around the camera spans over a thousand of the dense near bins, so the lanes split the wave's range rather than walk it in turn.
    const uint WaveFirstBin = WaveActiveMin(FirstBin);
    const uint WaveLastBin  = WaveActiveMax(LastBin);
    const uint Lanes        = WaveGetLaneCount();
    for (uint Base = WaveFirstBin; Base <= WaveLastBin && WaveFirstBin <= WaveLastBin; Base += Lanes)
    {
        const uint ZBin    = Base + WaveGetLaneIndex();
        uint       Lowest  = 0xFFFFFFFFu;
        uint       Highest = 0u;
        for (uint Other = 0u; Other < Lanes; ++Other)
        {
            const uint OtherFirst  = WaveReadLaneAt(FirstBin, Other);
            const uint OtherLast   = WaveReadLaneAt(LastBin, Other);
            const uint OtherSorted = WaveReadLaneAt(Sorted, Other);
            if (ZBin >= OtherFirst && ZBin <= OtherLast)
            {
                Lowest  = min(Lowest, OtherSorted);
                Highest = max(Highest, OtherSorted);
            }
        }
        if (ZBin <= WaveLastBin && Lowest <= Highest)
        {
            InterlockedMin(LightGrid().At(LIGHT_GRID_ZBINS + 2u * ZBin)[0], Lowest);
            InterlockedMin(LightGrid().At(LIGHT_GRID_ZBINS + 2u * ZBin + 1u)[0], ~Highest);
        }
    }

    if (!bValid)
    {
        return;
    }

    const uint  MaskWords = LightGridWord(LIGHT_GRID_MASK_WORDS);
    const uint  Word      = Sorted >> 5u;
    const uint  Bit       = 1u << (Sorted & 31u);
    const uint  GridX     = max(Scene().GridSize.x, 1u);
    const uint4 Rect      = SphereTileRect(ViewCenter, Depth, PositionRadius.w, ViewForward);
    for (uint TileY = Rect.y; TileY <= Rect.w; ++TileY)
    {
        for (uint TileX = Rect.x; TileX <= Rect.z; ++TileX)
        {
            const uint Index = (TileX + TileY * GridX) * MaskWords + Word;
            if (LightTileMasks().Contains(Index))
            {
                InterlockedOr(LightTileMasks().At(Index)[0], Bit);
            }
        }
    }
}
