#include "Includes/SurfaceShading.slang"
#ifdef TRANSLUCENT
#include "Includes/MomentOIT.slang"
#endif

// Forward base-pass pixel shader. The fragment input is the shared FBaseVertexOutput
// (MeshletGeometry.slang), produced identically by the VS-emulation and mesh-shader geometry paths. All
// lighting lives in ShadeSurface, shared with the deferred VisBuffer material pass so the two can't drift.
//
// Translucent materials compile twice for MBOIT (Includes/MomentOIT.slang): MOMENT_GENERATION runs the
// opacity graph alone and accumulates moments; the default variant shades, weighted by them.

struct PSOutput
{
    #if defined(TRANSLUCENT) && defined(MOMENT_GENERATION)

    float   MomentB0    : SV_Target0;
    float4  MomentB     : SV_Target1;

    #elif defined(TRANSLUCENT)

    float4  Accum       : SV_Target0;
    uint    Picker      : SV_Target1;

    #else

    float4 Color        : SV_Target0;
    uint Picker         : SV_Target1;

    #endif
};

// Masked materials `discard` inside ShadeSurface, which must run before the depth write -- [earlydepthstencil]
// would have already committed depth for a cutout texel, corrupting holes for fog, the HZB and decals.
// Opaque keeps forced early-Z over the heavy PBR shader.
#ifndef MASKED
[earlydepthstencil]
#endif
[shader("fragment")]
PSOutput FragmentMain(FBaseVertexOutput Input)
{
    // Per-instance constants arrive flat from the VS -- no per-pixel 128B FGPUInstance load. These locals
    // are the names the material pixel graph codegen reads.
    uint EntityID           = Input.EntityID;
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = normalize(Input.NormalWS.xyz);
    float3 ViewPosition     = Input.FragPos.xyz;
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0              = Input.UV;
    float2 UV1              = Input.UV1;
    float4 VertexColor      = Input.FragColor;

    // Tangent frame + UV gradients: part of the graph local contract, for nodes that ray-march in tangent
    // space or pick their own mip. Raster passes have a real 2x2 quad; the deferred lane substitutes
    // analytic derivatives.
    // (Do NOT write the material token in a comment: the compiler substitutes the FIRST occurrence.)
    float4 WorldTangent     = Input.TangentWS;
    float2 UV0_DDX          = ddx(UV0);
    float2 UV0_DDY          = ddy(UV0);
    float2 UV1_DDX          = ddx(UV1);
    float2 UV1_DDY          = ddy(UV1);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    $MATERIAL_INPUTS

    PSOutput Output;

#if defined(TRANSLUCENT) && defined(MOMENT_GENERATION)

    float Coverage = ComputeSurfaceCoverage(Input, Material);

    // Zero absorbance contributes nothing; discarding keeps the ROP traffic off two wide moment targets.
    // Safe under [earlydepthstencil] because the translucent passes only read depth.
    if (Coverage < 1e-4)
    {
        discard;
    }

    // FragPos is view space (+Z forward), so its z is the linear depth the shading pass also warps.
    float GenViewZ = max(abs(Input.FragPos.z), GetNearPlane());
    MomentGenerate(MomentWarpDepth(GenViewZ), MomentAbsorbance(Coverage), Output.MomentB0, Output.MomentB);

    return Output;

#else

    FShadedSurface Shaded = ShadeSurface(Input, Material);

#ifdef TRANSLUCENT

    // Opacity plus the Fresnel energy the surface reflects away: what glass mirrors never reaches the
    // background either. Mirrors ComputeSurfaceCoverage, which fed the moments.
    float Coverage = saturate(Shaded.Alpha + (1.0 - Shaded.Alpha) * Shaded.ReflectionCoverage);

    // Sentinel-guarded like SampleShadowMask: indexing the bindless table with ~0u is a device loss, not
    // an artifact. MakeSecondaryViewGlobals publishes these for captures and probe bakes too.
    uint  ZerothIndex   = Scene().MomentZerothIndex;
    float Transmittance = 1.0;
    if (ZerothIndex != 0xFFFFFFFFu)
    {
        int2  MomentCoords = int2(Input.Position.xy);
        float B0           = gTextures2D[ZerothIndex].Load(int3(MomentCoords, 0)).r;
        if (B0 >= MOMENT_MIN_ABSORBANCE)
        {
            float4 B      = gTextures2D[Scene().MomentsIndex].Load(int3(MomentCoords, 0));
            float  ViewZ  = max(abs(Input.FragPos.z), GetNearPlane());
            Transmittance = MomentTransmittanceAtDepth(B0, B, MomentWarpDepth(ViewZ));
        }
    }

    // Transmission is premultiplied by alpha; Reflection stays full strength (glass attenuates what is
    // behind it, not its own specular). Sanitize before the additive accumulation, where one bad fragment
    // takes the whole pixel. The clamp bounds 63 layers under fp16 max by construction.
    float3 Premultiplied = SanitizeRadiance(Shaded.Color.rgb * Shaded.Alpha + Shaded.Reflection);
    float3 Weighted      = min(Premultiplied * Transmittance, 1024.0);

    Output.Accum = float4(Weighted, Coverage);

    // View Mode > OIT Layer Count: 1.0 per fragment turns accum.a into a per-pixel fragment count,
    // separating an unstable fragment SET from unstable fragment VALUES.
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && Scene().CullData.DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
    {
        Output.Accum = float4(0.0, 0.0, 0.0, 1.0);
    }

    Output.Picker = EntityID;
    return Output;

#else

    Output.Color  = float4(Shaded.Color, Shaded.Alpha);
    Output.Picker = EntityID;
    return Output;

#endif // TRANSLUCENT

#endif // MOMENT_GENERATION
}
