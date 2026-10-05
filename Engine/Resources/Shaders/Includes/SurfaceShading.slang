#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/MeshletGeometry.slang"
#include "Includes/GBuffer.slang"
#include "Includes/IBLRuntime.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/ParallaxOcclusion.slang"
#include "Includes/HashedAlpha.slang"
#include "Includes/DistanceField.slang"

// Two halves so the deferred lanes can run them in separate passes: BuildGBufferData (material) and
// ShadeGBuffer (lighting). SHADING_COMPUTE has no rasterizer -- nothing it reaches may use ddx/ddy.
#ifndef TRANSLUCENT
#include "Includes/DBuffer.slang"
#endif

// Specialized per-pipeline (constant ids 1-3). Default ENABLED so unspecialized pipelines and offline
// slangc stay bit-for-bit; specializing to 0 dead-strips the guarded block AND its resource access.
[vk::constant_id(1)] const uint SPEC_ENABLE_DEBUG_VIEWS = 1;
[vk::constant_id(2)] const uint SPEC_ENABLE_DECALS      = 1;
[vk::constant_id(3)] const uint SPEC_ENABLE_GTAO        = 1;

// Frame-uniform light loop features, so a frame without them specializes to 0 and sheds their registers.
[vk::constant_id(10)] const uint SPEC_LOCAL_SHADOWS      = 1;
[vk::constant_id(11)] const uint SPEC_LOCAL_CONTACT      = 1;

// Whether any material on screen uses the model, so the deferred lighting dispatch drops the branch and its registers otherwise.
[vk::constant_id(18)] const uint SPEC_SHADING_CLEARCOAT  = 1;
[vk::constant_id(19)] const uint SPEC_SHADING_FOLIAGE    = 1;

// Hash-to-color for the meshlet debug view; stable per-meshlet hues across frames.
float3 MeshletDebugColor(uint Index)
{
    uint H = Index * 2654435761u;
    H ^= H >> 16;
    H *= 2246822519u;
    H ^= H >> 13;
    H *= 3266489917u;
    H ^= H >> 16;

    return float3(
        float( H         & 0xFFu) / 255.0,
        float((H >>  8)  & 0xFFu) / 255.0,
        float((H >> 16)  & 0xFFu) / 255.0
    );
}

float3 FresnelSchlick(float CosTheta, float3 F0)
{
    float x = saturate(1.0 - CosTheta);
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2 * x);
}

// Half-precision Schlick Fresnel for the light loop. CosTheta and F0 are both [0,1], so fp16 is ample
// and the (1-cos)^5 chain stays well within range.
half3 FresnelSchlickHalf(half CosTheta, half3 F0)
{
    half x  = saturate(half(1.0) - CosTheta);
    half x2 = x * x;
    return F0 + (half3(1.0) - F0) * (x2 * x2 * x);
}

// MikkTSpace tangent, Gram-Schmidt'd against the interpolated normal; TangentWS.w preserves the
// bitangent across UV mirrors. The frame lives in ParallaxOcclusion.slang so POM and decode share it.
float3 GetWorldNormal(float3 FragNormal, float4 FragTangent, float3 TangentSpaceNormal)
{
    // The frame's third row is the normal itself, so a flat map lands on it; a constant flat map folds the frame away.
    if (all(TangentSpaceNormal == float3(0.0, 0.0, 1.0)))
    {
        return FragNormal;
    }

    bool bDegenerate;
    float3x3 TBN = BuildTangentFrame(FragNormal, FragTangent, bDegenerate);
    if (bDegenerate)
    {
        // No usable tangent: keep the geometric normal rather than rotating the map into an arbitrary
        // frame, whose axis flips across the |N.z| ~ 1 pole and would seam the surface.
        return FragNormal;
    }
    return normalize(mul(TangentSpaceNormal, TBN));
}

// Geometric specular antialiasing (Kaplanyan 2016). Widens roughness by the pixel-footprint variance of
// the shading normal, so a normal varying faster than one pixel stops sparkling and settles into a lobe.
float ApplyGeometricSpecularAA(float Roughness, float3 dNdx, float3 dNdy)
{
    const float ScreenVariance = 0.25;
    const float Threshold      = 0.18;

    const float Variance = ScreenVariance * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    const float Kernel   = min(2.0 * Variance, Threshold);

    // Roughness squared is the GGX alpha the variance adds into; the widening is not linear in roughness.
    const float Alpha = Roughness * Roughness;
    return sqrt(saturate(Alpha + Kernel));
}

// Godot's positional "Soft Low" filter, 4 taps within 2 atlas texels.
static const int   kLocalShadowTaps          = 4;
static const float kLocalShadowRadiusTexels  = 2.0;

// One disk rotation per pixel, shared by every local light's shadow so the light loop pays for a single sincos.
float2 LocalShadowNoiseDir(float2 ScreenPos)
{
    float2 Dir;
    sincos(InterleavedGradientNoise(ScreenPos) * TWO_PI, Dir.y, Dir.x);
    return Dir;
}

// The cube face's projection in closed form, the same numbers its view-projection matrix produces without loading it.
float3 ProjectToCubeFace(float3 D, uint Face, float Radius)
{
    float3 P;
    switch (Face)
    {
    case 0u:  P = float3( D.z,  D.y,  D.x); break;
    case 1u:  P = float3(-D.z,  D.y, -D.x); break;
    case 2u:  P = float3(-D.x, -D.z,  D.y); break;
    case 3u:  P = float3(-D.x,  D.z, -D.y); break;
    case 4u:  P = float3(-D.x,  D.y,  D.z); break;
    default:  P = float3( D.x,  D.y, -D.z); break;
    }
    // Matches the shadow face projection, whose near plane is 1% of the radius with a 10 cm floor.
    const float Near     = max(Radius * 0.01, 0.1);
    const float InvRange = 1.0 / (Radius - Near);
    const float InvDepth = 1.0 / max(P.z, 1e-6);
    return float3(P.xy * InvDepth, Radius * InvRange - Radius * Near * InvRange * InvDepth);
}

float ComputeLocalShadow(FLight Light, float3 FragmentPos, float3 Normal, float2 NoiseDir)
{
    const bool   bSpot          = HasFlag(Light.Flags, ELightFlags::Spot);
    const float3 LightToFrag    = FragmentPos - Light.Position;
    const float3 LightDirToFrag = -normalize(LightToFrag);

    uint Face = 0u;
    if (!bSpot)
    {
        const float3 A = abs(LightToFrag);
        Face = (A.x >= A.y && A.x >= A.z) ? (LightToFrag.x >= 0.0 ? 0u : 1u)
             : (A.y >= A.z)               ? (LightToFrag.y >= 0.0 ? 2u : 3u)
             :                              (LightToFrag.z >= 0.0 ? 4u : 5u);
    }

    const FLightShadow ShadowTile = GetShadowTile(Light.ShadowDataIndex, Face);
    if (ShadowTile.ShadowMapIndex < 0)
    {
        return 1.0;
    }

    uint AtlasW, AtlasH;
    ShadowAtlas().GetDimensions(AtlasW, AtlasH);
    const float2 TexelUV = 1.0 / float2(AtlasW, AtlasH);

    // Normal offset scaled to one shadow texel in world units, so grazing walls stop self-shadowing.
    const float  AtlasTileTexels = max(ShadowTile.AtlasUVScale.x * float(AtlasW), 1.0);
    const float  WorldTexel      = (Light.Radius * 2.0) / AtlasTileTexels;
    const float  NdotL           = saturate(dot(Normal, LightDirToFrag));
    const float3 OffsetPos       = FragmentPos + Normal * (WorldTexel * 1.4 * saturate(1.0 - NdotL));

    // Constant, slope and tile-quantization terms, since a fixed bias tuned for 4096 texels self-shadows at 512.
    const float SlopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    const float Bias        = 0.00005 + 0.0003 * SlopeFactor + 1.5 / AtlasTileTexels;

    float3 ProjCoords;
    if (bSpot)
    {
        const float4 ShadowCoord = mul(GetShadowViewProjection(Light.ShadowDataIndex, 0u), float4(OffsetPos, 1.0));
        ProjCoords = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    }
    else
    {
        ProjCoords = ProjectToCubeFace(OffsetPos - Light.Position, Face, Light.Radius);
    }

    if (ProjCoords.z < 0.0 || ProjCoords.z > 1.0)
    {
        return 1.0;
    }

    const float  RefDepth = ProjCoords.z - Bias;
    const float2 BaseUV   = ShadowTile.AtlasUVOffset + saturate(ProjCoords.xy * 0.5 + 0.5) * ShadowTile.AtlasUVScale;
    // Taps clamp half a texel inside the face rect so the comparison footprint never reads a neighboring tile.
    const float2 TileMin  = ShadowTile.AtlasUVOffset + TexelUV * 0.5;
    const float2 TileMax  = ShadowTile.AtlasUVOffset + ShadowTile.AtlasUVScale - TexelUV * 0.5;
    const float2 RadiusUV = TexelUV * kLocalShadowRadiusTexels;

    const float2 GoldenStep = float2(cos(kVogelGoldenAngle), sin(kVogelGoldenAngle));
    float2 Dir    = NoiseDir;
    float  Shadow = 0.0;
    [unroll]
    for (int i = 0; i < kLocalShadowTaps; ++i)
    {
        const float2 Offset = Dir * (RadiusUV * sqrt((float(i) + 0.5) / float(kLocalShadowTaps)));
        Shadow += ShadowAtlas().SampleCmpLevelZero(clamp(BaseUV + Offset, TileMin, TileMax), RefDepth);
        Dir = RotateVogel(Dir, GoldenStep);
    }
    return Shadow * (1.0 / float(kLocalShadowTaps));
}

float ComputeShadowFactor(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition, float2 ScreenPos, bool ReceiveShadow)
{
    if (!ReceiveShadow)
    {
        return 1.0;
    }
    if (HasFlag(Light.Flags, ELightFlags::Directional))
    {
        return ComputeDirectionalShadow(Light, FragmentPos, Normal, ViewPosition, ScreenPos);
    }
    return ComputeLocalShadow(Light, FragmentPos, Normal, LocalShadowNoiseDir(ScreenPos));
}

static const float kLocalContactReachMeters = 0.15;
static const uint  kLocalContactSteps       = 8u;

// Kept out of ComputeShadowFactor, which inlines at two sites; only local lights ever reach this one.
float LocalContactShadow(FLight Light, float3 FragmentPos, float3 Normal, float2 ScreenPos)
{
    const float Reach = kLocalContactReachMeters;
    const uint  Steps = kLocalContactSteps;

    const float3 ViewP = mul(GetCameraView(), float4(FragmentPos, 1.0)).xyz;
    const float3 ViewN = NormalWorldToView(Normal);
    const float3 ViewL = normalize(NormalWorldToView(normalize(Light.Position - FragmentPos)));

    return ContactShadow(ViewP, ViewN, ViewL, InterleavedGradientNoise(ScreenPos), Reach, Steps);
}

// Debug visualizers stay out of the compute lane; the deferred renderer resolves them in SceneDebugViewPixel.slang.
#ifndef SHADING_COMPUTE

// Walk the light table once to find the sun's shadow-data slot. Used by the
// cascade debug visualizer; pixel-shader cost is irrelevant in that mode.
int FindSunShadowDataIndex()
{
    if (LightData().bHasSun == 0u)
    {
        return INDEX_NONE;
    }
    uint Count = GetNumLights();
    for (uint i = 0; i < Count; ++i)
    {
        FLight L = GetLightAt(i);
        if (HasFlag(L.Flags, ELightFlags::Directional) && L.ShadowDataIndex != INDEX_NONE)
        {
            return L.ShadowDataIndex;
        }
    }
    return INDEX_NONE;
}

// Per-cascade tint for the Shadow Cascades view, mirroring ComputeShadowFactor's selection.
// Dark gray = no sun shadow data; muted tint = fragment outside the selected cascade's bounds.
float3 CascadeDebugColor(float3 FragmentPos, float3 ViewPosition, float3 ShadedBase)
{
    int SunShadowIdx = FindSunShadowDataIndex();
    if (SunShadowIdx == INDEX_NONE)
    {
        return float3(0.08, 0.08, 0.08);
    }

    int CascadeIndex = 0;
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (ViewPosition.z > LightData().CascadeSplits[i])
            CascadeIndex = i + 1;
    }
    if (CascadeIndex >= NUM_CASCADES)
    {
        return float3(0.18, 0.18, 0.18);
    }

    float4x4 CascadeVP  = GetShadowViewProjection(SunShadowIdx, CascadeIndex);
    float4   ShadowCoord = mul(CascadeVP, float4(FragmentPos, 1.0));
    float3   ProjCoords  = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2   ProjUV      = ProjCoords.xy * 0.5 + 0.5;
    bool     Inside      = !(any(ProjUV < 0.0) || any(ProjUV > 1.0) ||
                              ProjCoords.z < 0.0 || ProjCoords.z > 1.0);

    const float3 CascadeTints[3] = {
        float3(1.00, 0.25, 0.25),
        float3(0.25, 1.00, 0.30),
        float3(0.30, 0.45, 1.00),
    };
    float3 Tint = CascadeTints[CascadeIndex];

    // Blend with a luminance proxy from the lit shading so silhouettes stay
    // readable while the cascade color dominates.
    float Luma = saturate(Luminance(ShadedBase));
    float3 Color = lerp(Tint * 0.4, Tint, 0.5 + 0.5 * Luma);

    if (!Inside)
    {
        Color *= 0.35;
    }
    return Color;
}

#endif // !SHADING_COMPUTE

// The roughness-dependent half of the specular lobe, rebuilt per light so each can raise the floor.
struct FSpecLobe
{
    float A2;            // GGX alpha squared, float so low-roughness highlights don't underflow
    float VisA;          // Smith height-correlated visibility, NdotL coefficient
    float VisB;          // Smith height-correlated visibility, constant term
};

// Heitz 2014 joint approximation, its 1 / (4 NdotV NdotL) denominator already folded in.
FSpecLobe MakeSpecLobe(float Roughness, float NdotV)
{
    const float Alpha = Roughness * Roughness;

    // Expanding the denominator to 2 NdotV NdotL (1 - a) + a (NdotV + NdotL) leaves NdotL as its only variable.
    FSpecLobe Lobe;
    Lobe.A2   = Alpha * Alpha;
    Lobe.VisA = 2.0 * NdotV * (1.0 - Alpha) + Alpha;
    Lobe.VisB = Alpha * NdotV;
    return Lobe;
}

float SpecVisibility(FSpecLobe Lobe, float NdotL)
{
    return 0.5 * rcp(NdotL * Lobe.VisA + Lobe.VisB);
}

// Terms that do not change across the light loop; BRDF weights are half to halve loop register pressure.
struct FShadingContext
{
    float3 N;
    float3 V;
    float  NdotV;        // max(dot(N,V), 1e-4)
    float  Roughness;    // surface value, before any per-light floor
    half3  F0;
    half3  DiffuseBase;  // Albedo * (1 - Metallic) / PI
    half3  EnergyComp;   // multi-scatter compensation (Turquin 2019)
};

// Distance window times spot cone, zero wherever the light cannot reach, which lets callers skip its shadow and BRDF.
float LightAttenuation(FLight Light, float3 L, float Distance)
{
    if (HasFlag(Light.Flags, ELightFlags::Directional))
    {
        return 1.0;
    }

    float Attenuation = 1.0 / max(Distance * Distance, 1e-8);
    float DistRatio   = Distance / Light.Radius;
    Attenuation      *= 1.0 - smoothstep(clamp(Light.Falloff, 0.0, 1.0), 1.0, DistRatio);

    if (HasFlag(Light.Flags, ELightFlags::Spot))
    {
        float3 LightDir = normalize(Light.Direction.xyz);
        float  CosTheta = dot(LightDir, L);
        Attenuation    *= clamp((CosTheta - Light.Angles.y) / max(Light.Angles.x - Light.Angles.y, 0.001), 0.0, 1.0);
    }
    return Attenuation;
}

// Shared by the base BRDF and the clearcoat lobe so the two layers cannot disagree.
float3 ComputeLightRadiance(FLight Light, float3 L, float Distance)
{
    return GetLightColor(Light).rgb * Light.Intensity * LightAttenuation(Light, L, Distance);
}

// One extra GGX layer over the base at a fixed dielectric F0 of 0.04. Deliberately NOT in
// FShadingContext: that is live for every pixel, this only inside the clearcoat branch.
float EvaluateClearcoatLobe(float3 N, float3 V, float3 L, float ClearcoatRoughness, out float OutFresnel)
{
    const float3 H     = normalize(V + L);
    const float  NdotH = saturate(dot(N, H));
    const float  NdotV = saturate(dot(N, V));
    const float  NdotL = saturate(dot(N, L));
    const float  LdotH = saturate(dot(L, H));

    // Clamped like the base lobe: a perfectly smooth coat is a delta the raster path cannot resolve.
    const float  Rough = max(ClearcoatRoughness, 0.045);
    const float  A     = Rough * Rough;
    const float  A2    = A * A;

    const float  D_Denom = NdotH * NdotH * (A2 - 1.0) + 1.0;
    const float  D       = A2 / max(PI * D_Denom * D_Denom, 1e-6);

    // Kelemen visibility: the cheap term the coat layer is normally paired with, and accurate enough
    // for a thin smooth layer where Smith's extra fidelity is invisible.
    const float  Vis = 0.25 / max(LdotH * LdotH, 1e-4);

    OutFresnel = 0.04 + 0.96 * pow(1.0 - LdotH, 5.0);
    return D * Vis * OutFresnel * NdotL;
}

// Thin-surface translucency (Barre-Brisebois and Bouchard, GDC 2011). Not a subsurface diffusion model,
// it is the view-dependent forward-scatter lobe that makes a backlit leaf glow. Peaks when the eye looks
// straight into the light through the surface, which is the only time real foliage does this.

// Depth of foliage the sun can still push light through, past which the lobe is fully extinguished.
static const float kFoliagePenetrationMeters = 0.5;

// A binary shadow test cannot feed this lobe, since a leaf's own front face is its occluder.
float SunTransmissionShadow(FLight Sun, float3 WorldPosition, bool bReceiveShadow)
{
    if (!bReceiveShadow)
    {
        return 1.0;
    }
    return 1.0 - saturate(SunOccluderDepthWS(Sun, WorldPosition) / kFoliagePenetrationMeters);
}

float3 EvaluateTransmission(FShadingContext S, float3 L, float3 Radiance, float3 Transmission, float Thickness)
{
    // Bending the sampled direction by the normal keeps the lobe from being a perfect mirror of the light,
    // which is what gives a curved leaf a soft gradient instead of one hot spot.
    const float Distortion = 0.2;
    const float Power      = 6.0;

    const float3 BackLight = normalize(L + S.N * Distortion);
    const float  Forward   = pow(saturate(dot(S.V, -BackLight)), Power);

    return Transmission * Thickness * Forward * Radiance;
}

// L is supplied by the caller for every light type, so the directional normalize happens once at the
// call site. Diffuse and specular return separately so translucency keeps reflections un-premultiplied.
void EvaluateRadianceContributionSplit(FLight Light, FShadingContext S, float3 L, float3 Radiance,
                                       out float3 OutDiffuse, out float3 OutSpecular)
{
    OutDiffuse  = float3(0.0);
    OutSpecular = float3(0.0);

    float NdotLf = max(dot(S.N, L), 0.0);
    if (NdotLf <= 0.0)
    {
        return;
    }

    float3 H        = normalize(S.V + L);

    // Rebuilt per light rather than cached, because branching on the floor cost 75 more instructions.
    const FSpecLobe Lobe = MakeSpecLobe(max(S.Roughness, GetLightMinRoughness(Light)), S.NdotV);

    // NDF stays full precision, NdotH near 1.0 needs the mantissa.
    float NdotH = max(dot(S.N, H), 0.0);
    float Denom = NdotH * NdotH * (Lobe.A2 - 1.0) + 1.0;
    float NDF   = Lobe.A2 / (PI * Denom * Denom);

    // Fresnel and the diffuse/spec tint are [0,1], so fp16 covers them.
    half  HdotV = (half)max(dot(H, S.V), 0.0);
    half3 F     = FresnelSchlickHalf(HdotV, S.F0);

    half3 SpecTint = F * S.EnergyComp;                // color part of the specular lobe
    half3 Diffuse  = (half3(1.0) - F) * S.DiffuseBase;

    // Recombine in float; NDF and the visibility term carry the HDR dynamic range between them.
    float3 Spec = float3(SpecTint) * (NDF * SpecVisibility(Lobe, NdotLf));

    OutDiffuse  = float3(Diffuse) * Radiance * NdotLf;
    OutSpecular = Spec * Radiance * NdotLf;
}

void EvaluateLightContributionSplit(FLight Light, FShadingContext S, float3 L, float Distance,
                                    out float3 OutDiffuse, out float3 OutSpecular)
{
    EvaluateRadianceContributionSplit(Light, S, L, ComputeLightRadiance(Light, L, Distance), OutDiffuse, OutSpecular);
}

float3 EvaluateRadianceContribution(FLight Light, FShadingContext S, float3 L, float3 Radiance)
{
    float3 Diffuse, Specular;
    EvaluateRadianceContributionSplit(Light, S, L, Radiance, Diffuse, Specular);
    return Diffuse + Specular;
}

float3 EvaluateLightContribution(FLight Light, FShadingContext S, float3 L, float Distance)
{
    return EvaluateRadianceContribution(Light, S, L, ComputeLightRadiance(Light, L, Distance));
}

// Result of shading a surface: the lit HDR color plus the material alpha. Callers pack their own
// render-target output (opaque -> Color/Picker, translucent -> transmittance-weighted Accum).
struct FShadedSurface
{
    float3 Color;       // opaque: full lit color. Translucent: transmission-modulated terms only (diffuse + ambient diffuse + emissive)
    float3 Reflection;  // translucent only: specular from lights + IBL, kept out of Color so it isn't attenuated by alpha. Zero for opaque.
    float  Alpha;
    // Translucent only: the fraction this surface reflects instead of transmitting. Added to coverage so
    // stacked glass darkens the background instead of summing reflections toward white.
    float  ReflectionCoverage;
};

// The material graph's output as GBuffer terms: tangent normal rotated to world, then DBuffer decals
// composited so they receive lights and shadows exactly like the surface under them.
FGBufferData BuildGBufferData(FBaseVertexOutput Input, FMaterialPixelInputs Material)
{
    FGBufferData G;
    G.BaseColor        = Material.Diffuse;
    G.AmbientOcclusion = Material.AmbientOcclusion;
    G.WorldNormal      = GetWorldNormal(normalize(Input.NormalWS.xyz), Input.TangentWS, Material.Normal);
    G.SelfShadow       = Material.SelfShadow;
    G.Roughness        = max(Material.Roughness, 0.045);
    G.Metallic         = Material.Metallic;
    G.Specular         = Material.Specular;
    G.Emissive         = Material.Emissive;
    G.bReceiveShadow   = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    G.bContactShadows  = (Input.InstanceFlags & (uint)EInstanceFlags::NoContactShadows) == 0u;

    G.Clearcoat          = Material.Clearcoat;
    G.ClearcoatRoughness = Material.ClearcoatRoughness;
    G.Transmission       = Material.Transmission;
    G.Thickness          = Material.Thickness;

    // Read at RUNTIME from the material flags, not a #define: models that only reinterpret GBuffer
    // channels do not justify a shader variant each.
    const uint MatFlags = GetMaterialFlags(Input.MaterialIndex);
    G.ShadingModel = GetMaterialShadingModel(MatFlags);

    // Unlit is a runtime test, so an instance that overrode its model is honored here too.
#ifndef TRANSLUCENT
    // The material or the instance can opt out, the latter for things that move through static decals.
    const bool bReceivesDecals = (MatFlags & MatFlag_NoDecals) == 0u && (Input.InstanceFlags & (uint)EInstanceFlags::NoDecals) == 0u;
    if (SPEC_ENABLE_DECALS != 0 && bReceivesDecals && !IsUnlit(G))
    {
        float2 DBufferUV = Input.Position.xy / float2(Scene().ScreenSize.xy);
        ApplyDBuffer(G.BaseColor, G.WorldNormal, G.Roughness, G.Metallic, G.AmbientOcclusion, G.Emissive, DBufferUV);
    }
#endif

#ifndef SHADING_COMPUTE
    // After the decals, which rewrite roughness and would otherwise blend the widening back out. Quad
    // derivatives see the mapped normal here, so this covers normal-map detail as well as curvature.
    G.Roughness = ApplyGeometricSpecularAA(G.Roughness, ddx(G.WorldNormal), ddy(G.WorldNormal));
#endif

    return G;
}

// The lighting sequence over an already-resolved surface, wherever it came from. Alpha is left to the
// caller -- it is a material property, not a GBuffer one.
FShadedSurface ShadeGBuffer(FGBufferData G, float3 WorldPosition, float3 ViewPosition, float2 ScreenPos)
{
    FShadedSurface Out;
    Out.Color              = float3(0.0);
    Out.Reflection         = float3(0.0);
    Out.ReflectionCoverage = 0.0;
    Out.Alpha              = 1.0;

    // Every lane checks the model at runtime, so an instance can override its way back to Lit. The cost
    // is that an unlit material's shader still contains the lighting path it branches over.
    if (IsUnlit(G))
    {
        Out.Color = G.BaseColor + G.Emissive;
        return Out;
    }

    // Bound to locals rather than read through G, so the light loop below is character-for-character the
    // code the forward path has always run.
    float3 N              = G.WorldNormal;
    float3 Albedo         = G.BaseColor;
    float  Roughness      = G.Roughness;
    float  Metallic       = G.Metallic;
    float  AO             = G.AmbientOcclusion;
    bool   bReceiveShadow = G.bReceiveShadow;
    bool   bContactShadows = G.bContactShadows;

    // Under ortho every ray is parallel, so the eye vector is constant rather than radiating from
    // the camera position; using the position would skew specular toward the viewport center.
    float3 V  = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);

    // Dielectric F0 = 0.08 * Specular (UE4 convention; default 0.5 -> 0.04). Metals override F0 with albedo.
    float3 F0_DIELECTRIC = float3(0.08 * G.Specular);
    float3 F0 = lerp(F0_DIELECTRIC, Albedo, Metallic);

    // Multi-scatter energy compensation factor (Turquin 2019), constant per pixel; tinted by F0
    // so colored metals get colored multi-bounce. Ess = single-scatter albedo (BRDF LUT scale+bias).
    float  NdotV_EC    = saturate(dot(N, V));
    float2 DFG_EC      = SampleBRDFLUT(NdotV_EC, Roughness);

#ifdef TRANSLUCENT
    // See FShadedSurface::ReflectionCoverage. Reuses the already-fetched BRDF LUT terms; the
    // FresnelSchlickRoughness call CSEs with the identical one inside EvaluateIBLAmbientSplit.
    float3 FssEssCov          = FresnelSchlickRoughness(NdotV_EC, F0, Roughness) * DFG_EC.x + DFG_EC.y;
    float  ReflectionCoverage = saturate(Luminance(FssEssCov));
#endif
    // EnergyComp is computed below, just before its only use, so this BRDF-LUT fetch's latency overlaps the
    // independent context ALU instead of stalling here.

    // Loop-invariant surface terms: hoisted out of the per-light BRDF so each light only re-derives the
    // L-dependent half. NdotV / roughness remap / view geometry term are identical across every light.
    FShadingContext Ctx;
    Ctx.N           = N;
    Ctx.V           = V;
    Ctx.NdotV       = max(NdotV_EC, 1e-4);
    Ctx.Roughness   = Roughness;
    Ctx.F0          = (half3)F0;
    Ctx.DiffuseBase = (half3)(Albedo * (1.0 - Metallic) * INV_PI);
    float3 EnergyComp = 1.0 + F0 * (1.0 / max(DFG_EC.x + DFG_EC.y, 1e-4) - 1.0);
    Ctx.EnergyComp  = (half3)EnergyComp;

    // Clamps to ZNear: fragments below it make log() negative, wrapping ZTile to a huge value that
    // overflows Clusters() -> garbage light list and an MMU fault.
    uint  ClusterIndex = ComputeClusterIndex(ViewPosition.z, ScreenPos);
    uint2 ClusterWords = GetClusterWordRange(ClusterIndex);

    // Clearcoat state. Hoisted out of the loops but only meaningful inside the branch; a non-clearcoat
    // pixel leaves Coat at 0 and every clearcoat block below is skipped whole.
    const bool  bClearcoat = SPEC_SHADING_CLEARCOAT != 0u && (G.ShadingModel == ShadingModel_Clearcoat);
    const float Coat       = bClearcoat ? saturate(G.Clearcoat) : 0.0;
    float3      LoCoat     = float3(0.0);

    // Gates the back-facing light paths below; every other model keeps its early-out intact.
    const bool  bFoliage   = SPEC_SHADING_FOLIAGE != 0u && (G.ShadingModel == ShadingModel_Foliage) && (G.Thickness > 0.0);

    float3 Lo = float3(0.0);
#ifdef TRANSLUCENT
    // Reflected specular is accumulated apart from transmission-modulated terms; the extra live
    // accumulator is translucent-only so the opaque loop's register footprint is untouched.
    float3 LoSpec = float3(0.0);
#endif

    if (LightData().bHasSun != 0)
    {
        FLight Light = GetLightAt(0);

        // Pixels facing away from the sun are self-shadowed, so they skip the cascade lookup; SunL feeds the BRDF too.
        float3 SunL    = normalize(Light.Direction.xyz);
        float  SunNdotL = dot(N, SunL);

        // Backlit foliage, which the lit path below early-outs on. Both lanes now read the cascade depth.
        if (bFoliage && SunNdotL <= 0.0)
        {
            const float BackShadow = SunTransmissionShadow(Light, WorldPosition, bReceiveShadow) * G.SelfShadow;
            Lo += EvaluateTransmission(Ctx, SunL, ComputeLightRadiance(Light, SunL, 0.0),
                                       G.Transmission, G.Thickness) * BackShadow;
        }

        if (SunNdotL > 0.0)
        {
            // Cascade shadow, then the material's own height-field self-occlusion. Applied here and not
            // to the whole light loop because the parallax trace only ever ran against the sun.
            float Shadow = ComputeShadowFactor(Light, WorldPosition, N, ViewPosition, ScreenPos, bReceiveShadow);
            Shadow *= G.SelfShadow;

            // A leaf lit from the front still passes some light forward at grazing view angles.
            if (bFoliage)
            {
                const float FrontShadow = SunTransmissionShadow(Light, WorldPosition, bReceiveShadow) * G.SelfShadow;
                Lo += EvaluateTransmission(Ctx, SunL, ComputeLightRadiance(Light, SunL, 0.0),
                                           G.Transmission, G.Thickness) * FrontShadow;
            }

            // Fully shadowed pixels get nothing from the sun, so they skip its BRDF.
            if (Shadow > 0.0)
            {
#ifdef TRANSLUCENT
                float3 SunDiffuse, SunSpec;
                EvaluateLightContributionSplit(Light, Ctx, SunL, 0.0, SunDiffuse, SunSpec);
                Lo     += SunDiffuse * Shadow;
                LoSpec += SunSpec * Shadow;
#else
                float3 SunBase = EvaluateLightContribution(Light, Ctx, SunL, 0.0);

                if (Coat > 0.0)
                {
                    // The coat reflects some light before it reaches the base, so the base loses exactly what the coat took.
                    float CoatF;
                    const float CoatSpec = EvaluateClearcoatLobe(N, V, SunL, G.ClearcoatRoughness, CoatF);
                    SunBase *= (1.0 - Coat * CoatF);
                    LoCoat  += ComputeLightRadiance(Light, SunL, 0.0) * (CoatSpec * Coat) * Shadow;
                }

                Lo += SunBase * Shadow;
#endif
            }
        }
    }

    const float2 ShadowNoiseDir = LocalShadowNoiseDir(ScreenPos);

    uint    NumLights   = GetNumLights();   // loop-invariant; hoisted out of the per-light test
    FLight* LightArray  = LightData().Lights.Data;
    const uint MaskWords = GetClusterMaskWords();

#ifdef SHADING_COMPUTE
    // The wave walks the union of its lanes' masks, so each light index is uniform and its fetch scalar.
    const uint FirstWord = WaveActiveMin(ClusterWords.x);
    const uint EndWord   = WaveActiveMax(ClusterWords.y);
#else
    const uint FirstWord = ClusterWords.x;
    const uint EndWord   = ClusterWords.y;
#endif

    [loop]
    for (uint Word = FirstWord; Word < EndWord; ++Word)
    {
        const bool bLaneWord = Word >= ClusterWords.x && Word < ClusterWords.y;
        const uint LaneMask  = bLaneWord ? GetClusterMaskWord(ClusterIndex, Word, MaskWords) : 0u;
#ifdef SHADING_COMPUTE
        uint Pending = WaveActiveBitOr(LaneMask);
#else
        uint Pending = LaneMask;
#endif

        [loop]
        while (Pending != 0u)
        {
            const uint Bit = firstbitlow(Pending);
            Pending &= Pending - 1u;

            const uint LightIndex  = Word * 32u + Bit;
            const bool bLaneListed = ((LaneMask >> Bit) & 1u) != 0u;

            // Wave-uniform on the scalar path, so rejecting a stale index never splits the wave.
            if (LightIndex >= NumLights)
            {
                continue;
            }

            // Position and Radius alone decide the cull; the rest of the light is fetched only once some lane wants it.
            const float4 PositionRadius = loadAligned<16>(reinterpret<float4*>(LightArray + LightIndex));
            const float3 LightToFrag    = PositionRadius.xyz - WorldPosition;
            const float  DistSq         = dot(LightToFrag, LightToFrag);

            // A listed light may reach only this pixel's back face, which contributes nothing except through foliage.
            const bool bBackFacing = dot(N, LightToFrag) <= 0.0;
            const bool bLit        = bLaneListed
                                  && (DistSq < PositionRadius.w * PositionRadius.w)
                                  && (!bBackFacing || bFoliage);

#ifdef SHADING_COMPUTE
            if (!WaveActiveAnyTrue(bLit))
#else
            if (!bLit)
#endif
            {
                continue;
            }

            const FLight Light       = loadAligned<16>(LightArray + LightIndex);
            const float  InvDist     = rsqrt(max(DistSq, 1e-8));
            const float3 L           = LightToFrag * InvDist;
            const float  DistToLight = DistSq * InvDist;

            // A lane outside the falloff or the spot cone gets nothing from this light, so it takes neither a shadow nor a BRDF.
            const float Attenuation = LightAttenuation(Light, L, DistToLight);
            if (bLit && Attenuation > 0.0)
            {
                const float3 Radiance = GetLightColor(Light).rgb * Light.Intensity * Attenuation;

                float Shadow = 1.0;
                if (SPEC_LOCAL_SHADOWS != 0 && Light.ShadowDataIndex != INDEX_NONE)
                {
                    Shadow = bReceiveShadow ? ComputeLocalShadow(Light, WorldPosition, bBackFacing ? -N : N, ShadowNoiseDir) : 1.0;
                }

                // Independent of the shadow map, so a light with no atlas tile can still ground what sits under it.
                if (SPEC_LOCAL_CONTACT != 0 && bReceiveShadow && bContactShadows && HasFlag(Light.Flags, ELightFlags::ContactShadow))
                {
                    Shadow = min(Shadow, LocalContactShadow(Light, WorldPosition, bBackFacing ? -N : N, ScreenPos));
                }

                if (Shadow > 0.0)
                {
                    if (bFoliage)
                    {
                        Lo += EvaluateTransmission(Ctx, L, Radiance, G.Transmission, G.Thickness) * Shadow;
                    }

#ifdef TRANSLUCENT
                    float3 LightDiffuse, LightSpec;
                    EvaluateRadianceContributionSplit(Light, Ctx, L, Radiance, LightDiffuse, LightSpec);
                    Lo     += LightDiffuse * Shadow;
                    LoSpec += LightSpec * Shadow;
#else
                    float3 LightBase = EvaluateRadianceContribution(Light, Ctx, L, Radiance);

                    if (Coat > 0.0)
                    {
                        float CoatF;
                        const float CoatSpec = EvaluateClearcoatLobe(N, V, L, G.ClearcoatRoughness, CoatF);
                        LightBase *= (1.0 - Coat * CoatF);
                        LoCoat    += Radiance * (CoatSpec * Coat) * Shadow;
                    }

                    Lo += LightBase * Shadow;
#endif
                }
            }
        }
    }

    // Attenuates ambient/IBL only; direct light has its own occlusion. TRANSLUCENT skips it entirely --
    // GTAO comes from OPAQUE depth, so at a glass pixel it holds the occlusion of what is behind it.
    float GTAO = 1.0;
#ifndef TRANSLUCENT
    // The compute lane tests at runtime, since deferred lighting does not specialize on GTAO and the fetch is one texel.
#ifdef SHADING_COMPUTE
    if (Scene().GTAOSettings.AOTextureIndex != 0xFFFFFFFFu)
#else
    if (SPEC_ENABLE_GTAO != 0 && Scene().GTAOSettings.AOTextureIndex != 0xFFFFFFFFu)
#endif
    {
        // Explicit LOD: this is a full-resolution screen-space texture read 1:1, and the deferred lighting
        // lane is compute, where an implicit derivative does not exist.
        float2 ScreenUV = ScreenPos / float2(Scene().ScreenSize.xy);
        GTAO = SampleTexture2DLevel(Scene().GTAOSettings.AOTextureIndex, SAMPLER_LINEAR_CLAMP, ScreenUV, 0.0).r;
    }
#endif

    // Folded into the AO the ambient is evaluated with, so specular takes it through ComputeSpecularOcclusion.
    const float AmbientAO = AO * GTAO;

    float3 Ambient;
    if (HasIBL())
    {
        // NdotV and DFG are recomputed here so the top-of-shader copies are not held across the light loop.
        float  NdotV_IBL  = saturate(dot(N, V));
        float2 DFG_IBL    = SampleBRDFLUT(NdotV_IBL, Roughness);
#ifdef TRANSLUCENT
        float3 IBLDiffuse, IBLSpec;
        EvaluateIBLAmbientSplit(WorldPosition, N, V, NdotV_IBL, DFG_IBL, Albedo, F0, Roughness, Metallic, AmbientAO, IBLDiffuse, IBLSpec);
        Ambient = IBLDiffuse * GetAmbientLightIntensity();
        LoSpec += IBLSpec * GetAmbientLightIntensity();
#else
        float3 IBLAmbient = EvaluateIBLAmbient(WorldPosition, N, V, NdotV_IBL, DFG_IBL, Albedo, F0, Roughness, Metallic, AmbientAO);
        Ambient = IBLAmbient * GetAmbientLightIntensity();
#endif
    }
    else
    {
        // No environment: a SkyLight alone still lights the scene via a flat diffuse ambient fill
        // (color * intensity from the SkyLight), occluded by material AO and GTAO. Metals carry no diffuse.
        float3 DiffuseColor = Albedo * (1.0 - Metallic);
        Ambient = GetAmbientLightColor() * GetAmbientLightIntensity() * DiffuseColor
                * AOMultiBounce(AmbientAO, DiffuseColor);
    }
    // The coat reflects the environment too -- most of the read on car paint. Same prefiltered cube at
    // the coat's roughness and fixed 0.04 F0, attenuating the ambient underneath for energy.
    if (Coat > 0.0 && HasIBL())
    {
        const float  CoatNdotV = saturate(dot(N, V));
        const float3 CoatR     = reflect(-V, N);
        const float  CoatRough = max(G.ClearcoatRoughness, 0.045);
        const float2 CoatDFG   = SampleBRDFLUT(CoatNdotV, CoatRough);

        const float3 CoatPrefiltered = SamplePrefilter(CoatR, CoatRough);
        const float  CoatF           = 0.04 + 0.96 * pow(1.0 - CoatNdotV, 5.0);
        const float3 CoatIBL         = CoatPrefiltered * (0.04 * CoatDFG.x + CoatDFG.y);

        Ambient *= (1.0 - Coat * CoatF);
        Lo      *= (1.0 - Coat * CoatF);
        LoCoat  += CoatIBL * Coat * GetAmbientLightIntensity()
                 * ComputeSpecularOcclusion(CoatNdotV, AmbientAO, CoatRough);
    }

    Out.Color = Ambient + Lo + LoCoat + G.Emissive;
#ifdef TRANSLUCENT
    Out.Reflection         = LoSpec;
    Out.ReflectionCoverage = ReflectionCoverage;
#endif
    return Out;
}

#ifndef SHADING_COMPUTE

// Surface identity a debug view needs but lighting does not, so it never reaches the GBuffer. The
// deferred debug pass re-reconstructs it from the VisBuffer.
struct FDebugSurfaceInfo
{
    float2 UV;
    float3 GeometricNormalWS;   // interpolated vertex normal, before the normal map
    uint   MeshletIndex;
    uint   MaterialIndex;

    // Pixels to the nearest triangle edge, for the wireframe overlay. Only the deferred lane can fill
    // this (TriangleEdgeDistancePixels, off the VisBuffer barycentrics); everything else passes
    // kNoEdgeDistance and simply draws no wire.
    float  EdgeDistancePixels;
};

static const float kNoEdgeDistance = 1e30;

// GBuffer.slang borrows channels per shading model: a clearcoat surface stores Clearcoat and
// ClearcoatRoughness in the slots every other model uses for SelfShadow and Specular. UnpackGBuffer
// hands the missing side back as the value LIGHTING would have used -- 0.5 specular, 1.0 self-shadow,
// 0.0 coat -- which is right for shading and a lie for a readout: a flat 0.5 over every coated surface
// reads as "authored 0.5", not "this channel does not exist here".
//
// So the four borrowed views paint the pixels their channel was not stored for. Violet = no value in
// this channel for this surface's model. Dark teal = the surface simply is not clearcoat, which is
// distinct from a coated surface whose strength is genuinely 0 (that one reads black).
//
// Caveat worth knowing: the forward lane (terrain, translucency) does not round-trip through
// Pack/Unpack, so those surfaces DO carry a real Specular even when coated. The marker keys off the
// shading model either way, so a coated forward pixel is flagged too -- deliberately, because the
// value it holds is not what the deferred lane would have shaded it with.
static const float3 kDebugNotStored = float3(0.35, 0.0, 0.5);
static const float3 kDebugNoCoat    = float3(0.0, 0.06, 0.08);

// Wireframe overlay. Cyan because it is the hue least likely to sit in a lit scene already -- the wire
// has to read over whatever it is drawn on, and this mode keeps the shading underneath.
static const float3 kWireframeColor      = float3(0.15, 1.0, 1.0);
static const float  kWireframeWidthPixels = 1.25;

// Editor view-mode overrides, applied on top of the shaded color. Callers gate on
// SPEC_ENABLE_DEBUG_VIEWS so game/shipping pipelines strip the whole switch.
float3 ApplyDebugView(uint DebugMode, float3 Color, FGBufferData G, FDebugSurfaceInfo Info,
                      float3 WorldPosition, float3 ViewPosition, float2 ScreenPos)
{
    float3 N = G.WorldNormal;
    float3 V = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);

    switch (DebugMode)
    {
    case DEBUG_MODE_UNLIT:
        Color = G.BaseColor + G.Emissive;
        break;
    case DEBUG_MODE_MESHLETS:
        Color = MeshletDebugColor(Info.MeshletIndex);
        break;
    case DEBUG_MODE_WORLD_NORMAL:
        Color = normalize(Info.GeometricNormalWS) * 0.5 + 0.5;
        break;
    case DEBUG_MODE_SHADING_NORMAL:
        Color = N * 0.5 + 0.5;
        break;
    case DEBUG_MODE_BASE_COLOR:
        Color = G.BaseColor;
        break;
    case DEBUG_MODE_ROUGHNESS:
        Color = float3(G.Roughness, G.Roughness, G.Roughness);
        break;
    case DEBUG_MODE_METALLIC:
        Color = float3(G.Metallic, G.Metallic, G.Metallic);
        break;
    case DEBUG_MODE_AMBIENT_OCCLUSION:
        Color = float3(G.AmbientOcclusion, G.AmbientOcclusion, G.AmbientOcclusion);
        break;
    case DEBUG_MODE_EMISSIVE:
        Color = G.Emissive;
        break;
    case DEBUG_MODE_UV:
        Color = float3(frac(Info.UV), 0.0);
        break;
    case DEBUG_MODE_LIGHT_COMPLEXITY:
    {
        uint DbgIndex = ComputeClusterIndex(ViewPosition.z, ScreenPos);
        Color = LightComplexityColor(GetClusterLightCount(DbgIndex));
        break;
    }
    case DEBUG_MODE_CLUSTER_GRID:
    {
        uint DbgIndex = ComputeClusterIndex(ViewPosition.z, ScreenPos);
        Color = ClusterDebugColor(DbgIndex, GetClusterLightCount(DbgIndex));
        break;
    }
    case DEBUG_MODE_SHADOW_CASCADES:
        Color = CascadeDebugColor(WorldPosition, ViewPosition, Color);
        break;
    case DEBUG_MODE_GTAO:
    {
        // Show the (blurred) screen-space AO factor. White where GTAO is off for this view.
        float DbgAO = 1.0;
        if (Scene().GTAOSettings.AOTextureIndex != 0xFFFFFFFFu)
        {
            float2 DbgUV = ScreenPos / float2(Scene().ScreenSize.xy);
            DbgAO = SampleTexture2DLevel(Scene().GTAOSettings.AOTextureIndex, SAMPLER_LINEAR_CLAMP, DbgUV, 0.0).r;
        }
        Color = float3(DbgAO, DbgAO, DbgAO);
        break;
    }
    case DEBUG_MODE_MATERIAL_ID:
        // Stable hue per GPU material slot -- shows which material owns each pixel (the deferred
        // material binning) and works for any surface (mesh or terrain).
        Color = MeshletDebugColor(Info.MaterialIndex);
        break;
    case DEBUG_MODE_SPECULAR:
        // Dielectric F0 control (0.5 = the 4% default). Not stored for clearcoat -- the coat drives
        // its own fixed F0 and lighting reads 0.5 regardless, so there is nothing to show.
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? kDebugNotStored
                    : float3(G.Specular, G.Specular, G.Specular);
        break;
    case DEBUG_MODE_SHADING_MODEL:
        // Categorical, not a ramp: this is the view that explains what the borrowed-channel views are
        // doing, so the three models have to be unmistakable rather than three shades of one hue.
        switch (G.ShadingModel)
        {
            case ShadingModel_Default:   Color = float3(0.25, 0.35, 0.55); break;   // steel blue
            case ShadingModel_Unlit:     Color = float3(0.95, 0.55, 0.10); break;   // orange
            case ShadingModel_Clearcoat: Color = float3(0.10, 0.85, 0.85); break;   // cyan
            default:                     Color = float3(1.0, 0.0, 1.0);    break;   // magenta: model not mapped here
        }
        break;
    case DEBUG_MODE_CLEARCOAT:
        // Black IS a meaningful reading here (a coated material dialled to zero), which is exactly why
        // uncoated surfaces get a hue instead of black -- otherwise the two are indistinguishable.
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? float3(G.Clearcoat, G.Clearcoat, G.Clearcoat)
                    : kDebugNoCoat;
        break;
    case DEBUG_MODE_CLEARCOAT_ROUGHNESS:
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? float3(G.ClearcoatRoughness, G.ClearcoatRoughness, G.ClearcoatRoughness)
                    : kDebugNoCoat;
        break;
    case DEBUG_MODE_SELF_SHADOW:
        // The material's own shadow term (POM / bent-normal occlusion), NOT the shadow map -- use
        // Shadow Cascades for that. Borrowed away by clearcoat, same as Specular.
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? kDebugNotStored
                    : float3(G.SelfShadow, G.SelfShadow, G.SelfShadow);
        break;
    case DEBUG_MODE_WIREFRAME_OVERLAY:
    {
        // The only mode that COMPOSITES: Color arrives as the lit shading and keeps it everywhere the
        // wire is not, which is the whole point -- wireframe ON the normally-drawn mesh, not instead
        // of it. Callers that cannot supply an edge distance leave the sentinel and draw nothing.
        const float Half = 0.5 * kWireframeWidthPixels;
        const float Wire = 1.0 - smoothstep(Half, Half + 1.0, Info.EdgeDistancePixels);
        Color = lerp(Color, kWireframeColor, Wire);
        break;
    }
    case DEBUG_MODE_PROBE_INFLUENCE:
    {
        // Status colors are FULLY SATURATED, covered pixels always PASTEL -- saturation, not hue, tells them
        // apart. Red = no probes, blue = array unallocated, yellow = none baked, magenta = outside all.
        FProbeDebugInfo Dbg = DebugReflectionProbes(WorldPosition);
        switch (Dbg.Status)
        {
            case kProbeStatusNoProbes:   Color = float3(1.0, 0.0, 0.0); break;
            case kProbeStatusNoArray:    Color = float3(0.0, 0.3, 1.0); break;
            case kProbeStatusNoneBaked:  Color = float3(1.0, 0.85, 0.0); break;
            case kProbeStatusOutsideAll: Color = float3(1.0, 0.0, 1.0); break;
            default:
                // ProbeDebugColor, not MeshletDebugColor: the latter hashes index 0 to pure black, so one probe on
                // slice 0 looked like no coverage. Brightness is floored so a barely-reaching probe keeps its hue.
                Color = ProbeDebugColor((uint)Dbg.DominantSlice) * (0.25 + 0.75 * Dbg.TotalWeight);
                break;
        }
        break;
    }
    case DEBUG_MODE_PROBE_RADIANCE:
    {
        // Divided back out by coverage so a half-covered pixel shows the capture at full brightness. Sky here
        // means the capture itself saw sky -- classically a room whose single-sided walls let it see through.
        FProbeSample DbgSample = SampleReflectionProbes(WorldPosition, reflect(-V, N), 0.0);
        Color = (DbgSample.Weight > 1e-3)
                    ? DbgSample.Radiance / DbgSample.Weight
                    : float3(0.0);
        break;
    }
    }

    return Color;
}

// Forward wrapper: alpha clip, both halves, then debug overrides. Terrain and translucency raster
// through this; opaque meshes run the two halves in separate deferred passes instead.
FShadedSurface ShadeSurface(FBaseVertexOutput Input, FMaterialPixelInputs Material)
{
    uint MatFlags = GetMaterialFlags(Input.MaterialIndex);
    if (HasFlag(MatFlags, MatFlag_Masked))
    {
        const float3 ClipAnchor = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
        const float  ClipCutoff = HashedAlphaThreshold(ClipAnchor, length(Input.FragPos.xyz),
                                                       GetMaterialOpacityClip(Input.MaterialIndex));
        if (Material.Opacity < ClipCutoff)
        {
            discard;
        }
    }

    FGBufferData G = BuildGBufferData(Input, Material);

    float3 ViewPosition  = Input.FragPos.xyz;
    float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;

    FShadedSurface Out = ShadeGBuffer(G, WorldPosition, ViewPosition, Input.Position.xy);
    Out.Alpha = Material.Opacity;

    // SPEC_ENABLE_DEBUG_VIEWS lets game/shipping pipelines strip the whole switch (DebugMode is always
    // NONE there) so there is no per-pixel branch and the PS stays small.
    uint DebugMode = Scene().CullData.DebugMode;
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && DebugMode != DEBUG_MODE_NONE)
    {
#ifdef TRANSLUCENT
        // Debug views replace Color wholesale; don't composite reflections on top. Unlit never wrote a
        // reflection in the first place, so zeroing it unconditionally is the same result.
        Out.Reflection = float3(0.0);
#endif
        FDebugSurfaceInfo Info;
        Info.UV                = Input.UV;
        Info.GeometricNormalWS = Input.NormalWS.xyz;
        Info.MeshletIndex      = Input.MeshletIndex;
        Info.MaterialIndex     = Input.MaterialIndex;
        // Forward lane (terrain, translucency): no VisBuffer triangle, so no wire on these surfaces.
        Info.EdgeDistancePixels = kNoEdgeDistance;

        Out.Color = ApplyDebugView(DebugMode, Out.Color, G, Info, WorldPosition, ViewPosition, Input.Position.xy);
    }

    return Out;
}

#endif // !SHADING_COMPUTE
