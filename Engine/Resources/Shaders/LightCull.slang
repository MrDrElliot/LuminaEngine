#pragma once
#include "Includes/SceneGlobals.slang"

// A sphere wholly outside one of the four side planes of the view projection never reaches the image.
bool SphereInsideSidePlanes(float3 Center, float Radius)
{
    const float4x4 ViewProj = mul(GetCameraProjection(), GetCameraView());
    const float4   Planes[4] = { ViewProj[3] + ViewProj[0], ViewProj[3] - ViewProj[0], ViewProj[3] + ViewProj[1], ViewProj[3] - ViewProj[1] };
    [unroll]
    for (uint Index = 0u; Index < 4u; ++Index)
    {
        if (dot(Planes[Index].xyz, Center) + Planes[Index].w < -Radius * length(Planes[Index].xyz))
        {
            return false;
        }
    }
    return true;
}

// One thread per retained slot keeps what this view can see, ranked inside its depth bin so the scatter sorts by depth for free.
[shader("compute")]
[numthreads(LIGHT_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const uint Slot = DispatchID.x;

    bool bVisible = false;
    uint Bin      = 0u;
    if (RetainedLightBounds().Contains(Slot))
    {
        const float4 Sphere = RetainedLightBounds().GetAligned(Slot);
        const uint   Layers = RetainedLightLayers().Contains(Slot) ? RetainedLightLayers().At(Slot)[0] : ~0u;
        if (Sphere.w > 0.0 && (Layers & LightData().ViewLayerMask) != 0u)
        {
            const float Depth = dot(WorldToView(Sphere.xyz), NormalWorldToView(GetCameraForward()));
            bVisible = Depth + Sphere.w > GetNearPlane() && Depth - Sphere.w < GetFarPlane()
                    && SphereInsideSidePlanes(Sphere.xyz, Sphere.w);
            Bin = LightZBin(Depth);
        }
    }

    // One atomic per wave claims the wave's run of visible entries.
    const uint WaveVisible = WaveActiveCountBits(bVisible);
    const uint Prefix      = WavePrefixCountBits(bVisible);
    uint Base = 0u;
    if (WaveIsFirstLane() && WaveVisible != 0u)
    {
        InterlockedAdd(LightGrid().At(LIGHT_GRID_COUNTER)[0], WaveVisible, Base);
    }
    Base = WaveReadLaneFirst(Base);

    if (bVisible)
    {
        uint Rank;
        InterlockedAdd(LightGrid().At(LIGHT_GRID_BIN_COUNTS + Bin)[0], 1u, Rank);
        const uint Entry = LIGHT_GRID_VISIBLE + 2u * (Base + Prefix);
        LightGrid().At(Entry)[0]      = Slot;
        LightGrid().At(Entry + 1u)[0] = (Bin << LIGHT_GRID_RANK_BITS) | Rank;
    }
}
