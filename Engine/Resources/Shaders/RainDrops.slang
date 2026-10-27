#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

struct FPushConstants
{
    GPUSpan<uint4> Visible;
    GPUSpan<uint>  Args;
    uint           NumDrops;
    float          Exposure;
    uint           bSnow;
    uint           _Pad0;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

bool OutsideSameSide(float4 A, float4 B)
{
    return (A.x >  A.w && B.x >  B.w) || (A.x < -A.w && B.x < -B.w)
        || (A.y >  A.w && B.y >  B.w) || (A.y < -A.w && B.y < -B.w)
        || (A.w <= 0.0 && B.w <= 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x == 0u)
    {
        Pass().Args.Set(0u, kRainStreakVertices);
    }

    const uint Index = DTid.x;
    if (Index >= Pass().NumDrops || !HasWeather())
    {
        return;
    }

    const FWeatherData W      = GetWeather();
    const float3       Camera = GetCameraPosition();
    const bool         bSnow  = Pass().bSnow != 0u;
    const FRainDrop    Drop   = bSnow ? EvaluateSnowFlake(W, Index, Camera) : EvaluateRainDrop(W, Index, Camera);

    const float Radius = bSnow ? W.SnowLook.z : W.RainTint.w;
    const float Dist   = length(Drop.Position - Camera);
    if (Dist > Radius || Dist < kRainNearClip)
    {
        return;
    }

    const float3 Tail = Drop.Position - Drop.Velocity * Pass().Exposure;
    if (OutsideSameSide(WorldToClip(Drop.Position), WorldToClip(Tail)))
    {
        return;
    }

    if (RainExposure(W, Drop.Position) <= 0.0)
    {
        return;
    }

    // Thins the field toward its edge so the wrap at the tile boundary never pops.
    const float Fade = saturate((Radius - Dist) / (Radius * 0.25)) * saturate((Dist - kRainNearClip) / 1.2);
    const float3 Light = RainLightAt(Drop.Position);

    uint Slot;
    InterlockedAdd(Pass().Args.At(1u)[0], 1u, Slot);
    Pass().Visible.Set(Slot, uint4(Index, PackHalf2(Light.rg), PackHalf2(float2(Light.b, Fade)), 0u));
}
