#pragma once

// Set on a slot whose impostor the pixel shader tests for coverage, which a sub-pixel quad skips.
static const uint kProxyAlphaTestBit = 0x80000000u;

struct FProxyVertexOutput
{
    nointerpolation uint VisID       : VISID;
    nointerpolation uint SlotAndTest : PROXY_SLOT;
    float3 WorldPos                  : WORLD_POS;
    float4 Position                  : SV_Position;
};
