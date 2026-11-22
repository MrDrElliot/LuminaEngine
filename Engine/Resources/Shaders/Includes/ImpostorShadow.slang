#pragma once
#include "VisBufferSurface.slang"
#include "Impostor.slang"
#include "VirtualShadow.slang"

struct FImpostorShadowArgs
{
    // CullImpostorPoints' shadow words, a level above each point index.
    GPUSpan<uint> List;
    int           ShadowDataBase;
    uint          _Pad0;
    uint          _Pad1;
    uint          _Pad2;
};
FImpostorShadowArgs* Pass() { return GetPassArgs<FImpostorShadowArgs>(); }

static const uint kImpostorShadowLevelShift = 27u;

// Share of the impostor's radius its shadow caster sits behind the baked surface, past the depth's quantization and compression error.
static const float kImpostorShadowDepthBias = 0.4;

struct FImpostorShadowVertexOutput
{
    nointerpolation uint Entry : PROXY_ENTRY;
    nointerpolation uint Level : SHADOW_LEVEL;
    float3 WorldPos            : WORLD_POS;
    float4 Position            : SV_Position;
};

// Six level projections ride each of the sun's shadow data slots.
float4x4 ImpostorShadowViewProjection(uint Level)
{
    return GetShadowViewProjection(Pass().ShadowDataBase + int(Level / 6u), Level % 6u);
}

// The point's model as the camera's proxy pass widens it, so a thinned survivor casts the shadow of the tree it shows.
float4x4 ImpostorShadowModel(FVisProxy P)
{
    float3 GroundWiden;
    ProxyWidenedCenter(P.Model, P.Proxy, GroundWiden);
    return ProxyWidenedModel(P.Model, GroundWiden);
}
