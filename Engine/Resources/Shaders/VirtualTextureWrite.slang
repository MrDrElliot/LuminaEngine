#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/VirtualTexture.slang"

// Stamps, decay and scroll clears for runtime virtual textures, picked by Mode.

#define VT_WRITE_CLEAR 0u
#define VT_WRITE_DECAY 1u
#define VT_WRITE_STAMP 2u

// Matches EVirtualTextureBlend.
#define VT_BLEND_MAX      0u
#define VT_BLEND_MIN      1u
#define VT_BLEND_ADD      2u
#define VT_BLEND_SUBTRACT 3u
#define VT_BLEND_REPLACE  4u

// Must match FVirtualTextureStampGPU in SceneRenderer_VirtualTexture.cpp.
struct FVirtualTextureStampGPU
{
    float2 From;
    float2 To;
    float4 Value;
    float  Radius;
    float  Hardness;
    float  Opacity;
    uint   Blend;
};

// Must match FVirtualTextureWriteArgs in SceneRenderer_VirtualTexture.cpp.
struct FVirtualTextureWriteArgs
{
    FRWTexture2D Target;
    uint         Mode;
    uint         Resolution;
    float        TexelSize;
    int2         RectMin;
    uint2        RectSize;
    float4       Clear;
    float        Amount;
    uint         StampTiles;
    int2         WindowOrigin;
    GPUSpan<FVirtualTextureStampGPU> Stamps;
};
FVirtualTextureWriteArgs* Pass() { return GetPassArgs<FVirtualTextureWriteArgs>(); }

float DistanceToSegment(float2 P, float2 A, float2 B)
{
    const float2 AB = B - A;
    const float  T  = saturate(dot(P - A, AB) / max(dot(AB, AB), 1e-8));
    return length(P - (A + AB * T));
}

float4 BlendStamp(float4 Dst, float4 Value, float Alpha, uint Blend)
{
    switch (Blend)
    {
    case VT_BLEND_MIN:      return lerp(Dst, min(Dst, Value), saturate(Alpha));
    case VT_BLEND_ADD:      return Dst + Value * Alpha;
    case VT_BLEND_SUBTRACT: return Dst - Value * Alpha;
    case VT_BLEND_REPLACE:  return lerp(Dst, Value, saturate(Alpha));
    default:                return lerp(Dst, max(Dst, Value), saturate(Alpha));
    }
}

void StampTexel(uint3 GroupID, uint2 Thread)
{
    const FVirtualTextureStampGPU S = Pass().Stamps.Get(GroupID.z);
    const float Texel = Pass().TexelSize;
    const int2  Min   = int2(floor((min(S.From, S.To) - S.Radius) / Texel));
    const int2  Max   = int2(ceil((max(S.From, S.To) + S.Radius) / Texel));
    const int2  World = Min + int2(GroupID.xy * 8u + Thread);
    const int2  Local = World - Pass().WindowOrigin;
    if (any(World >= Max) || any(Local < 0) || any(Local >= int(Pass().Resolution)))
    {
        return;
    }

    const float2 XZ    = (float2(World) + 0.5) * Texel;
    const float  D     = DistanceToSegment(XZ, S.From, S.To);
    const float  Core  = S.Radius * S.Hardness;
    const float  Fall  = 1.0 - smoothstep(Core, max(S.Radius, Core + 1e-4), D);
    if (Fall <= 0.0)
    {
        return;
    }

    const uint2 Px = VirtualTextureImageTexel(World, Pass().Resolution);
    Pass().Target[Px] = BlendStamp(Pass().Target[Px], S.Value, Fall * S.Opacity, S.Blend);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GroupID : SV_GroupID, uint3 Thread : SV_GroupThreadID)
{
    switch (Pass().Mode)
    {
    case VT_WRITE_STAMP:
        StampTexel(GroupID, Thread.xy);
        return;

    case VT_WRITE_DECAY:
        if (all(DTid.xy < Pass().Resolution))
        {
            Pass().Target[DTid.xy] = lerp(Pass().Target[DTid.xy], Pass().Clear, Pass().Amount);
        }
        return;

    default:
        if (all(DTid.xy < Pass().RectSize))
        {
            Pass().Target[VirtualTextureImageTexel(Pass().RectMin + int2(DTid.xy), Pass().Resolution)] = Pass().Clear;
        }
        return;
    }
}
