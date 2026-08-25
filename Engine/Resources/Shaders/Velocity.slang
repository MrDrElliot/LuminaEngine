#pragma once
#include "Includes/SceneGlobals.slang"

// Camera-only motion from depth; anything moving in world space reads as static here.

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FData
{
    uint DepthIndex;
};

// Any depth on the ray reconstructs the same direction, so this one is picked for float range, not distance.
static const float kRayProbeDepth = 0.5;

float2 VelocityFromClip(float4 PrevClip, float2 UV)
{
    // Behind the previous eye, so no history pixel exists to point at.
    if (PrevClip.w <= 0.0)
    {
        return float2(0.0, 0.0);
    }

    return ((PrevClip.xy / PrevClip.w) * 0.5 + 0.5) - UV;
}

[shader("fragment")]
float2 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    const float Raw = gTextures2D[Data.DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], Input.UV, 0.0).r;

    // Reverse-Z puts the far plane at exactly 0, where a position unprojects past float precision.
    if (Raw <= 0.0)
    {
        const float3 ViewRay  = ReconstructViewPos(Input.UV, kRayProbeDepth, GetInverseCameraProjection());
        const float3 WorldRay = mul((float3x3)GetInverseCameraView(), ViewRay);

        // Zero w drops the translation column, which is what a point at infinity should ignore.
        return VelocityFromClip(mul(GetPrevViewProjection(), float4(WorldRay, 0.0)), Input.UV);
    }

    // Both matrices carry this frame's jitter, so the unprojection is exact for the depth that was rendered.
    const float3 ViewPos  = ReconstructViewPos(Input.UV, Raw, GetInverseCameraProjection());
    const float4 WorldPos = mul(GetInverseCameraView(), float4(ViewPos, 1.0));

    return VelocityFromClip(mul(GetPrevViewProjection(), WorldPos), Input.UV);
}
