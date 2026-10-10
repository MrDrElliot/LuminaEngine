#pragma once
#include "Common.slang"
#include "SceneGlobals.slang"

// Each target's alpha holds the product of (1 minus coverage) over all decals, so surface * alpha + rgb composites the layer.

// Call before lighting so decals receive lights, shadows and IBL.
void ApplyDBuffer(inout float3 Albedo, inout float3 WorldNormal, inout float Roughness,
                  inout float Metallic, inout float AO, inout float3 Emissive, float2 ScreenUV)
{
    if (!Scene().DBufferA.IsValid())
    {
        return;
    }

    FSampler Smp = Samplers::PointClamp;
    float4   A = Scene().DBufferA.SampleLevel(Smp, ScreenUV, 0.0);
    float4   B = Scene().DBufferB.SampleLevel(Smp, ScreenUV, 0.0);
    float4   C = Scene().DBufferC.SampleLevel(Smp, ScreenUV, 0.0);
    Emissive += Scene().DBufferD.SampleLevel(Smp, ScreenUV, 0.0).rgb;

    if (A.a < 0.9999)
    {
        Albedo = Albedo * A.a + A.rgb;
    }

    // Composited in octahedral space, approximate but stable.
    if (B.a < 0.9999)
    {
        float2 OctSurface   = OctEncode01(WorldNormal);
        float2 OctComposite = B.rg + OctSurface * B.a;
        WorldNormal = OctDecode01(OctComposite);
    }

    if (C.a < 0.9999)
    {
        Roughness = saturate(Roughness * C.a + C.r);
        Metallic  = saturate(Metallic  * C.a + C.g);
        AO        = saturate(AO        * C.a + C.b);
    }
}
