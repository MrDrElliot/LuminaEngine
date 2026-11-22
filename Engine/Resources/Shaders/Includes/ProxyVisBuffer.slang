#pragma once

struct FProxyVertexOutput
{
    nointerpolation uint VisID      : VISID;
    // The proxy list entry the pixel shader tests the impostor's coverage of, when bAlphaTest is set.
    nointerpolation uint Entry      : PROXY_ENTRY;
    nointerpolation uint bAlphaTest : PROXY_ALPHA_TEST;
    float3 WorldPos                 : WORLD_POS;
    float4 Position                 : SV_Position;
};
