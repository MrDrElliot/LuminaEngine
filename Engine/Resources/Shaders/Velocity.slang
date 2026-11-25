#pragma once
#include "Includes/SceneGlobals.slang"

// Camera-only motion from depth; anything moving in world space reads as static here.

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Mirror of FData in SceneRenderer_Post.cpp.
struct FData
{
    // Previous view projection times this view's inverse view and inverse projection.
    float4x4   ClipToPrevClip;
    // Previous view projection times this view's inverse view.
    float4x4   ViewToPrevClip;
    FTexture2D Depth;
    uint       _Pad0;
    uint       _Pad1;
    uint       _Pad2;
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

    const float Raw = Data.Depth.SampleLevel(Samplers::PointClamp, Input.UV, 0.0).r;

    // Reverse-Z puts the far plane at exactly 0, where a position unprojects past float precision.
    if (Raw <= 0.0)
    {
        const float3 ViewRay = ReconstructViewPos(Input.UV, kRayProbeDepth, GetInverseCameraProjection());

        // Zero w drops the translation column, which is what a point at infinity should ignore.
        return VelocityFromClip(mul(Data.ViewToPrevClip, float4(ViewRay, 0.0)), Input.UV - GetTemporalJitterUV());
    }

    // Homogeneous all the way, so the previous clip position comes out scaled by this view's positive w, which the divide cancels.
    const float4 Clip = float4(Input.UV * 2.0 - 1.0, Raw, 1.0);
    return VelocityFromClip(mul(Data.ClipToPrevClip, Clip), Input.UV - GetTemporalJitterUV());
}
