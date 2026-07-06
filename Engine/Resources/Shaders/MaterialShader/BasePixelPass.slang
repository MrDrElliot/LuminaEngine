#include "Includes/SurfaceShading.slang"

// Forward base-pass pixel shader. The fragment input is the shared FBaseVertexOutput (MeshletGeometry.slang),
// produced identically by the VS-emulation and mesh-shader geometry paths. All lighting lives in
// ShadeSurface (SurfaceShading.slang), shared with the deferred VisBuffer material pass so the two can't
// drift; this file only reconstructs the material-graph locals, runs the pixel graph, and packs output.

struct PSOutput
{
    #ifdef TRANSLUCENT

    float4  Accum       : SV_Target0;
    float   Revealage   : SV_Target1;
    uint    Picker      : SV_Target2;

    #else

    float4 Color        : SV_Target0;
    uint Picker         : SV_Target1;

    #endif
};

// Masked materials run the alpha-test `discard` (inside ShadeSurface), which MUST execute before the depth
// write -- so the masked variant omits early depth tests. With [earlydepthstencil] the depth write happens
// before the shader, so a discarded cutout texel would still write depth (corrupting holes for fog, the HZB,
// decals, etc.). Opaque/non-masked keeps forced early-Z over the heavy PBR shader; its discard branch is
// never taken at runtime, so the early write is always correct.
#ifndef MASKED
[earlydepthstencil]
#endif
[shader("fragment")]
PSOutput FragmentMain(FBaseVertexOutput Input)
{
    // Per-instance constants come in flat from the VS -- no per-pixel 128B FGPUInstance global load.
    // These locals are the names the material pixel graph codegen reads.
    uint EntityID           = Input.EntityID;
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = normalize(Input.NormalWS.xyz);
    float3 ViewPosition     = Input.FragPos.xyz;
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0              = Input.UV;
    float4 VertexColor      = Input.FragColor;

    $MATERIAL_INPUTS

    FShadedSurface Shaded = ShadeSurface(Input, Material);

    PSOutput Output;

#ifdef TRANSLUCENT

    // WBOIT weight (McGuire & Bavoil 2013, eq. 9), driven by LINEAR view depth so nearer fragments
    // dominate the average. SV_Position.z is reverse-Z and non-linear -- the old formula built on it
    // both inverted the ordering and saturated its own clamp, degenerating into an unweighted average
    // (the classic uniform milky look). FragPos is view space (+Z forward), so its z is the linear depth.
    float ViewZ  = max(abs(Input.FragPos.z), GetNearPlane());
    float Z5     = ViewZ * (1.0 / 5.0);
    float Z200   = ViewZ * (1.0 / 200.0);
    float Z200_2 = Z200 * Z200;

    // Coverage = opacity plus the Fresnel energy the surface reflects away; what glass mirrors never
    // reaches the background, so stacked glasses attenuate each other's reflections instead of summing
    // them toward white. For a single surface the resolve still reconstructs exactly Color * a +
    // Reflection over Background * (1 - Coverage).
    float Coverage = saturate(Shaded.Alpha + (1.0 - Shaded.Alpha) * Shaded.ReflectionCoverage);

    // Weight clamp is 3e2 (not the paper's 3e3): accum is RGBA16F, and weighted HDR radiance summed
    // over a few layers must stay under fp16 max (65504) or pixels flip through the resolve's isinf
    // fallback as the camera moves (heavy flicker).
    float Weight = Coverage * clamp(10.0 / (1e-5 + Z5 * Z5 + Z200_2 * Z200_2 * Z200_2), 1e-2, 3e2);

    // Transmission is premultiplied by alpha; Reflection stays full strength (glass attenuates what's
    // BEHIND it, not its own specular). Sanitize kills NaN/INF from shading before they poison the
    // additive accum (the resolve's isinf() does NOT catch NaN).
    //
    // The per-surface radiance (Premultiplied / Coverage) is Karis range-compressed (FireflyTonemap)
    // BEFORE weighting and un-compressed after the resolve's ratio. Two properties this buys:
    //  1. fp16 overflow is impossible with no per-fragment cap (compressed values are <= ~1, so 100+
    //     layers fit under 65504). A cap on rgb ALONE is poison: it pins the numerator while the
    //     weight-sum denominator keeps moving, so ANY per-frame weight variation strobes bright pixels.
    //  2. rgb and alpha now carry the IDENTICAL Coverage * Weight factor, so the resolved ratio is
    //     exactly the compressed radiance -- the displayed color is invariant to uniform per-fragment
    //     weight wobble by construction.
    float3 Premultiplied = SanitizeRadiance(Shaded.Color.rgb * Shaded.Alpha + Shaded.Reflection);
    float3 Compressed    = FireflyTonemap(Premultiplied / max(Coverage, 1e-3));
    Output.Accum     = float4(Compressed * (Coverage * Weight), Coverage * Weight);
    Output.Revealage = Coverage;

    // Layer-count instrumentation (View Mode > OIT Layer Count): exactly 1.0 per fragment turns
    // accum.a into a raw per-pixel fragment count (fp16-exact to 2048), so a strobing SET of
    // fragments is directly distinguishable from strobing fragment VALUES. Stripped from
    // game/shipping pipelines with the rest of the debug views.
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && Scene().CullData.DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
    {
        Output.Accum = float4(0.0, 0.0, 0.0, 1.0);
    }

#else

    Output.Color = float4(Shaded.Color, Shaded.Alpha);

#endif // TRANSLUCENT

    Output.Picker = EntityID;
    return Output;
}
