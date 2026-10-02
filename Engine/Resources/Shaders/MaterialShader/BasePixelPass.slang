#include "Includes/SurfaceShading.slang"
#include "Includes/Fog.slang"

// Forward base-pass pixel shader. The fragment input is the shared FBaseVertexOutput
// (MeshletGeometry.slang), produced identically by the VS-emulation and mesh-shader geometry paths. All
// lighting lives in ShadeSurface, shared with the deferred VisBuffer material pass so the two can't drift.

// Translucent materials accumulate into weighted blended OIT targets, which OITResolve composites over the scene.
struct PSOutput
{
    #if defined(TRANSLUCENT)

    float4  Accum       : SV_Target0;
    float   Revealage   : SV_Target1;
    uint    Picker      : SV_Target2;

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
    float3 WorldPositionExcludingOffsets = Input.WorldPosNoWPO;
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
    float3 WorldPositionExcludingOffsets_DDX = ddx(WorldPositionExcludingOffsets);
    float3 WorldPositionExcludingOffsets_DDY = ddy(WorldPositionExcludingOffsets);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    // One report per pixel, keyed by material slot -- see RequestMaterialResolution. A real pixel shader,
    // so these are the quad's implicit derivatives rather than analytic ones.
    RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
    $MATERIAL_INPUTS

    PSOutput Output;

    FShadedSurface Shaded = ShadeSurface(Input, Material);

#ifdef TRANSLUCENT

    // Opacity plus the Fresnel energy the surface reflects away, since what glass mirrors never reaches the background either.
    float Coverage = saturate(Shaded.Alpha + (1.0 - Shaded.Alpha) * Shaded.ReflectionCoverage);

    // Transmission is premultiplied by alpha; Reflection stays full strength (glass attenuates what is
    // behind it, not its own specular). Sanitize before the additive accumulation, where one bad fragment
    // takes the whole pixel. The clamp bounds 63 layers under fp16 max by construction.
#ifdef ALPHA_COMPOSITE
    // The graph already emitted premultiplied color, so weighting it again would darken it twice.
    float3 Premultiplied = SanitizeRadiance(Shaded.Color.rgb + Shaded.Reflection);
#else
    float3 Premultiplied = SanitizeRadiance(Shaded.Color.rgb * Shaded.Alpha + Shaded.Reflection);
#endif

    // Coverage-weighted since the rest of the pixel was fogged already, and additive takes attenuation alone.
    float4 FogTerm   = Fog_EvaluateForTranslucent(Input.FragPos.xyz, Input.Position.xy);
    bool   bAdditive = (GetMaterialFlags(MaterialIndex) & MatFlag_Additive) != 0u;
    Premultiplied    = Premultiplied * FogTerm.a + (bAdditive ? float3(0.0) : FogTerm.rgb * Coverage);

    // McGuire and Bavoil's depth weight, divided by its maximum so the fp16 sum cannot overflow.
    const float ViewZ  = abs(Input.FragPos.z);
    const float Weight = clamp(10.0 / (1e-5 + pow(ViewZ / 5.0, 2.0) + pow(ViewZ / 200.0, 6.0)), 1e-2, 3e3) / 3e3;

    Output.Accum     = float4(min(Premultiplied, 1024.0) * Weight, Coverage * Weight);
    Output.Revealage = Coverage;

    // View Mode > OIT Layer Count, where 1.0 per fragment turns accum.a into a per-pixel fragment count.
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && Scene().CullData.DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
    {
        Output.Accum     = float4(0.0, 0.0, 0.0, 1.0);
        Output.Revealage = 0.0;
    }

    Output.Picker = EntityID;
    return Output;

#else

#ifdef MODULATE
    // Blended as DstColor * Src, so opacity has to fade the color toward the multiplicative identity.
    Output.Color = float4(lerp(float3(1.0), SanitizeRadiance(Shaded.Color), saturate(Shaded.Alpha)), 1.0);
#else
    Output.Color  = float4(Shaded.Color, Shaded.Alpha);
#endif
    Output.Picker = EntityID;
    return Output;

#endif // TRANSLUCENT
}
