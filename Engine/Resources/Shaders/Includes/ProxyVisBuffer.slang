#pragma once

struct FProxyVertexOutput
{
    nointerpolation uint   VisID       : VISID;
    // The impostor's meshlet header, whose atlases the pixel shader samples when bAlphaTest is set.
    nointerpolation uint   HeaderSlot  : PROXY_HEADER;
    nointerpolation uint   bAlphaTest  : PROXY_ALPHA_TEST;
    // The three views the quad blends, each frame packed as x | y << 16, with their weights and the mip, all constant across the quad.
    nointerpolation uint3  ViewFrames  : PROXY_VIEW_FRAMES;
    nointerpolation float3 ViewWeights : PROXY_VIEW_WEIGHTS;
    nointerpolation float  Mip         : PROXY_MIP;
    float3 WorldPos                    : WORLD_POS;
    // The camera ray in the instance's space, unnormalized, which is affine in the world position and so interpolates exactly.
    float3 LocalOrigin                 : PROXY_LOCAL_ORIGIN;
    float3 LocalDir                    : PROXY_LOCAL_DIR;
    float4 Position                    : SV_Position;
};

void PackImpostorViews(FImpostorViews Views, out uint3 Frames, out float3 Weights)
{
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        Frames[i] = Views.Frame[i].x | (Views.Frame[i].y << 16u);
    }
    Weights = Views.Weights;
}

FImpostorViews UnpackImpostorViews(uint3 Frames, float3 Weights)
{
    FImpostorViews Views;
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        Views.Frame[i] = uint2(Frames[i] & 0xFFFFu, Frames[i] >> 16u);
    }
    Views.Weights = Weights;
    return Views;
}
