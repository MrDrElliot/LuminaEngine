#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/MeshletGeometry.slang"
#include "Includes/GBuffer.slang"
#include "Includes/IBLRuntime.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/ParallaxOcclusion.slang"
#include "Includes/HashedAlpha.slang"
#include "Includes/DistanceField.slang"

// SHADING_COMPUTE has no rasterizer, so nothing it reaches may use ddx or ddy.
#ifndef TRANSLUCENT
#include "Includes/DBuffer.slang"
#endif

// Specialized per pipeline; 0 dead-strips the guarded block and its resource access.
[vk::constant_id(1)] const uint SPEC_ENABLE_DEBUG_VIEWS = 1;
[vk::constant_id(2)] const uint SPEC_ENABLE_DECALS      = 1;
[vk::constant_id(3)] const uint SPEC_ENABLE_GTAO        = 1;

// Frame-uniform light loop features, so a frame without them specializes to 0 and sheds their registers.
[vk::constant_id(10)] const uint SPEC_LOCAL_SHADOWS      = 1;
[vk::constant_id(11)] const uint SPEC_LOCAL_CONTACT      = 1;

// Whether any material on screen uses the model, so the deferred lighting dispatch drops the branch and its registers otherwise.
[vk::constant_id(18)] const uint SPEC_SHADING_CLEARCOAT  = 1;
[vk::constant_id(19)] const uint SPEC_SHADING_FOLIAGE    = 1;

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

// CosTheta and F0 are both in [0,1], so fp16 is ample.
half3 FresnelSchlickHalf(half CosTheta, half3 F0)
{
    half x  = saturate(half(1.0) - CosTheta);
    half x2 = x * x;
    return F0 + (half3(1.0) - F0) * (x2 * x2 * x);
}

// The frame is built in ParallaxOcclusion.slang so POM and normal decode share it.
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
        // Keep the geometric normal, since an arbitrary frame flips across the |N.z| ~ 1 pole and would seam.
        return FragNormal;
    }
    return normalize(mul(TangentSpaceNormal, TBN));
}

// Geometric specular antialiasing (Kaplanyan 2016).
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

// Sun occlusion traced past the cascades from the opaque depth, so translucency never reads it.
float SunFarShadowMask(float2 ScreenPos)
{
// A forward masked surface writes its own depth after the trace, so its pixels describe whatever was behind it.
#if !defined(TRANSLUCENT) && !(defined(MASKED) && !defined(SHADING_COMPUTE))
    const FTexture2D Mask = Scene().GTAOSettings.SunShadowMask;
    if (Mask.IsValid())
    {
        return Mask.Load(int3(int2(ScreenPos), 0)).r;
    }
#endif
    return 1.0;
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

// Pixel-shader cost is irrelevant here, only the cascade debug view calls it.
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

// Dark gray means no sun shadow data, a muted tint means outside the selected cascade's bounds.
float3 CascadeDebugColor(float3 FragmentPos, float3 ViewPosition, float3 ShadedBase)
{
    int SunShadowIdx = FindSunShadowDataIndex();
    if (SunShadowIdx == INDEX_NONE)
    {
        return float3(0.08, 0.08, 0.08);
    }

    const int NumActive = ActiveCascadeCount();
    int CascadeIndex = 0;
    for (int i = 0; i < NumActive; ++i)
    {
        if (ViewPosition.z > LightData().CascadeSplits[i])
            CascadeIndex = i + 1;
    }
    if (CascadeIndex >= NumActive)
    {
        return float3(0.18, 0.18, 0.18);
    }

    float4x4 CascadeVP  = GetShadowViewProjection(SunShadowIdx, CascadeIndex);
    float4   ShadowCoord = mul(CascadeVP, float4(FragmentPos, 1.0));
    float3   ProjCoords  = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2   ProjUV      = ProjCoords.xy * 0.5 + 0.5;
    bool     Inside      = !(any(ProjUV < 0.0) || any(ProjUV > 1.0) ||
                              ProjCoords.z < 0.0 || ProjCoords.z > 1.0);

    const float3 CascadeTints[NUM_CASCADES] = {
        float3(1.00, 0.25, 0.25),
        float3(0.25, 1.00, 0.30),
        float3(0.30, 0.45, 1.00),
        float3(1.00, 0.85, 0.25),
    };
    float3 Tint = CascadeIndex >= NearCascadeCount() ? float3(0.85, 0.30, 1.00) : CascadeTints[CascadeIndex];

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

    // Expanding the denominator leaves NdotL as its only variable.
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
    half3  DiffuseBase;  // Albedo * (1-Metallic) / PI
    half3  EnergyComp;   // multi-scatter compensation (Turquin 2019)
};

// Distance window times spot cone, zero wherever the light cannot reach, which lets callers skip its shadow and BRDF.
float LightAttenuation(FLight Light, float3 L, float Distance)
{
    if (HasFlag(Light.Flags, ELightFlags::Directional))
    {
        return 1.0;
    }

    // The rectangle integral already carries the distance, so an area light takes only its window and its front face.
    if (HasFlag(Light.Flags, ELightFlags::Area))
    {
        const float Window = 1.0 - smoothstep(clamp(Light.Falloff, 0.0, 1.0), 1.0, Distance / Light.Radius);
        return dot(Light.Direction.xyz, L) > 0.0 ? Window : 0.0;
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

// Hill and Heitz's fit of theta over sin(theta), so a polygon edge integrates without acos.
float3 IntegrateRectEdge(float3 A, float3 B)
{
    const float CosTheta = dot(A, B);
    const float AbsCos   = abs(CosTheta);
    const float Fit      = (0.8543985 + (0.4965155 + 0.0145206 * AbsCos) * AbsCos)
                         / (3.4175940 + (4.1616724 + AbsCos) * AbsCos);
    const float ThetaOverSin = CosTheta > 0.0 ? Fit : 0.5 * rsqrt(max(1.0 - CosTheta * CosTheta, 1e-7)) - Fit;
    return cross(A, B) * ThetaOverSin;
}

// An area light split into a diffuse and a specular direction, each with the radiance a point light there would carry.
struct FAreaLightTerms
{
    float3 DiffuseL;
    float3 DiffuseRadiance;
    float3 SpecularL;
    float3 SpecularRadiance;
};

// Radiance is the light's color and intensity after its window, spread here over the rectangle's area.
FAreaLightTerms EvaluateAreaLight(FLight Light, float3 Radiance, float3 P, float3 N, float3 V, float Roughness)
{
    const float3 Forward  = -normalize(Light.Direction.xyz);
    const float3 Right    = UnpackNormal(asuint(Light.VolumetricScatteringRadius));
    const float3 Up       = cross(Forward, Right);
    const float2 HalfSize = max(Light.Angles, float2(1e-3, 1e-3));
    const float  Area     = 4.0 * HalfSize.x * HalfSize.y;
    const float3 Emitted  = Radiance / Area;
    const float3 ToCenter = Light.Position - P;

    const float3 X  = Right * HalfSize.x;
    const float3 Y  = Up * HalfSize.y;
    const float3 C0 = normalize(ToCenter - X - Y);
    const float3 C1 = normalize(ToCenter + X - Y);
    const float3 C2 = normalize(ToCenter + X + Y);
    const float3 C3 = normalize(ToCenter - X + Y);
    float3 FormFactor = 0.5 * (IntegrateRectEdge(C0, C1) + IntegrateRectEdge(C1, C2) + IntegrateRectEdge(C2, C3) + IntegrateRectEdge(C3, C0));
    FormFactor = dot(FormFactor, ToCenter) < 0.0 ? -FormFactor : FormFactor;

    FAreaLightTerms Terms;
    Terms.DiffuseL        = FormFactor * rsqrt(max(dot(FormFactor, FormFactor), 1e-12));
    Terms.DiffuseRadiance = Emitted * (max(dot(FormFactor, N), 0.0) / max(dot(N, Terms.DiffuseL), 1e-3));

    // The reflection ray's hit on the plane, clamped into the rectangle, stands in for the whole light.
    const float3 Reflected = reflect(-V, N);
    const float  RdotF     = dot(Reflected, Forward);
    const float  Depth     = dot(-ToCenter, Forward);
    const float  RayT      = RdotF < -1e-4 ? Depth / -RdotF : length(ToCenter);
    const float3 Local     = P + Reflected * RayT - Light.Position;
    const float3 OnRect    = Light.Position + Right * clamp(dot(Local, Right), -HalfSize.x, HalfSize.x)
                                            + Up * clamp(dot(Local, Up), -HalfSize.y, HalfSize.y);
    const float3 ToRect    = OnRect - P;
    const float  DistSq    = max(dot(ToRect, ToRect), 1e-8);
    Terms.SpecularL        = ToRect * rsqrt(DistSq);

    // Karis's widening, with the rectangle as the disk of equal area, keeps the lobe's energy as it spreads.
    const float Alpha     = max(Roughness * Roughness, 1e-3);
    const float WideAlpha = saturate(Alpha + sqrt(Area / PI) * 0.5 * rsqrt(DistSq));
    const float Energy    = (Alpha / WideAlpha) * (Alpha / WideAlpha);
    Terms.SpecularRadiance = Emitted * (Area * max(dot(-Terms.SpecularL, Forward), 0.0) / DistSq * Energy);
    return Terms;
}

// Shared by the base BRDF and the clearcoat lobe so the two layers cannot disagree.
float3 ComputeLightRadiance(FLight Light, float3 L, float Distance)
{
    return GetLightColor(Light).rgb * Light.Intensity * LightAttenuation(Light, L, Distance);
}

// Kept out of FShadingContext, which is live for every pixel, so it only costs inside the clearcoat branch.
float EvaluateClearcoatLobe(float3 N, float3 V, float3 L, float ClearcoatRoughness, out float OutFresnel)
{
    const float3 H     = normalize(V + L);
    const float  NdotH = saturate(dot(N, H));
    const float  NdotV = saturate(dot(N, V));
    const float  NdotL = saturate(dot(N, L));
    const float  LdotH = saturate(dot(L, H));

    // A perfectly smooth coat is a delta the raster path cannot resolve.
    const float  Rough = max(ClearcoatRoughness, 0.045);
    const float  A     = Rough * Rough;
    const float  A2    = A * A;

    const float  D_Denom = NdotH * NdotH * (A2 - 1.0) + 1.0;
    const float  D       = A2 / max(PI * D_Denom * D_Denom, 1e-6);

    // Kelemen visibility, accurate enough for a thin smooth layer.
    const float  Vis = 0.25 / max(LdotH * LdotH, 1e-4);

    OutFresnel = 0.04 + 0.96 * pow(1.0 - LdotH, 5.0);
    return D * Vis * OutFresnel * NdotL;
}

// Thin-surface translucency (Barre-Brisebois and Bouchard, GDC 2011), the forward-scatter lobe that makes a backlit leaf glow.

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
    // Bending by the normal gives a curved leaf a soft gradient instead of one hot spot.
    const float Distortion = 0.2;
    const float Power      = 6.0;

    const float3 BackLight = normalize(L + S.N * Distortion);
    const float  Forward   = pow(saturate(dot(S.V, -BackLight)), Power);

    return Transmission * Thickness * Forward * Radiance;
}

// Frostbite's energy-renormalized Burley term, so rough surfaces brighten and smooth ones darken toward grazing.
float BurleyDiffuseScale(float NdotL, float NdotV, float LdotH, float Roughness)
{
    const float EnergyBias   = 0.5 * Roughness;
    const float EnergyFactor = lerp(1.0, 1.0 / 1.51, Roughness);
    const float Fd90         = EnergyBias + 2.0 * LdotH * LdotH * Roughness;
    const float L5           = 1.0 - NdotL;
    const float V5           = 1.0 - NdotV;
    const float LightScatter = 1.0 + (Fd90 - 1.0) * (L5 * L5) * (L5 * L5) * L5;
    const float ViewScatter  = 1.0 + (Fd90 - 1.0) * (V5 * V5) * (V5 * V5) * V5;
    return LightScatter * ViewScatter * EnergyFactor;
}

// Diffuse and specular return separately so translucency keeps reflections un-premultiplied.
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

    half3 SpecTint = F * S.EnergyComp;
    half3 Diffuse  = S.DiffuseBase * (half)BurleyDiffuseScale(NdotLf, S.NdotV, (float)HdotV, S.Roughness);

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

struct FShadedSurface
{
    float3 Color;       // translucent holds only the transmission-modulated terms (diffuse, ambient diffuse, emissive)
    float3 Reflection;  // translucent specular, kept out of Color so alpha does not attenuate it
    float  Alpha;
    // Translucent reflectance added to coverage so stacked glass darkens instead of summing reflections toward white.
    float  ReflectionCoverage;
};

// DBuffer decals composite here so they receive lights and shadows like the surface under them.
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

    // Read from the material flags at runtime, since models that only reinterpret GBuffer channels do not justify a variant each.
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
    // After the decals, which rewrite roughness and would otherwise blend the widening back out.
    G.Roughness = ApplyGeometricSpecularAA(G.Roughness, ddx(G.WorldNormal), ddy(G.WorldNormal));
#endif

    return G;
}

FShadedSurface ShadeGBuffer(FGBufferData G, float3 WorldPosition, float3 ViewPosition, float2 ScreenPos)
{
    FShadedSurface Out;
    Out.Color              = float3(0.0);
    Out.Reflection         = float3(0.0);
    Out.ReflectionCoverage = 0.0;
    Out.Alpha              = 1.0;

    // The model is checked at runtime so an instance can override its way back to Lit.
    if (IsUnlit(G))
    {
        Out.Color = G.BaseColor + G.Emissive;
        return Out;
    }

    float3 N              = G.WorldNormal;
    float3 Albedo         = G.BaseColor;
    float  Roughness      = G.Roughness;
    float  Metallic       = G.Metallic;
    float  AO             = G.AmbientOcclusion;
    bool   bReceiveShadow = G.bReceiveShadow;
    bool   bContactShadows = G.bContactShadows;

    // Under ortho every ray is parallel, so the eye vector is constant.
    float3 V  = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);

    // Dielectric F0 = 0.08 * Specular (UE4 convention; default 0.5 -> 0.04). Metals override F0 with albedo.
    float3 F0_DIELECTRIC = float3(0.08 * G.Specular);
    float3 F0 = lerp(F0_DIELECTRIC, Albedo, Metallic);

    // Multi-scatter energy compensation (Turquin 2019), tinted by F0 so colored metals get colored multi-bounce.
    float  NdotV_EC    = saturate(dot(N, V));
    float2 DFG_EC      = SampleBRDFLUT(NdotV_EC, Roughness);

#ifdef TRANSLUCENT
    float3 FssEssCov          = FresnelSchlickRoughness(NdotV_EC, F0, Roughness) * DFG_EC.x + DFG_EC.y;
    float  ReflectionCoverage = saturate(Luminance(FssEssCov));
#endif
    // EnergyComp is computed later so this LUT fetch's latency overlaps the context ALU.

    FShadingContext Ctx;
    Ctx.N           = N;
    Ctx.V           = V;
    Ctx.NdotV       = max(NdotV_EC, 1e-4);
    Ctx.Roughness   = Roughness;
    Ctx.F0          = (half3)F0;
    Ctx.DiffuseBase = (half3)(Albedo * (1.0 - Metallic) * INV_PI);
    float3 EnergyComp = 1.0 + F0 * (1.0 / max(DFG_EC.x + DFG_EC.y, 1e-4) - 1.0);
    Ctx.EnergyComp  = (half3)EnergyComp;

    // Clamps to ZNear, since a fragment below it wraps ZTile past Clusters() and faults the MMU.
    uint  ClusterIndex = ComputeClusterIndex(ViewPosition.z, ScreenPos);
    uint2 ClusterWords = GetClusterWordRange(ClusterIndex);

    // A non-clearcoat pixel leaves Coat at 0 and skips every clearcoat block.
    const bool  bClearcoat = SPEC_SHADING_CLEARCOAT != 0u && (G.ShadingModel == ShadingModel_Clearcoat);
    const float Coat       = bClearcoat ? saturate(G.Clearcoat) : 0.0;
    float3      LoCoat     = float3(0.0);

    // Gates the back-facing light paths below; every other model keeps its early-out intact.
    const bool  bFoliage   = SPEC_SHADING_FOLIAGE != 0u && (G.ShadingModel == ShadingModel_Foliage) && (G.Thickness > 0.0);

    float3 Lo = float3(0.0);
#ifdef TRANSLUCENT
    // Translucent-only accumulator, so the opaque loop's register footprint is untouched.
    float3 LoSpec = float3(0.0);
#endif

    if (LightData().bHasSun != 0)
    {
        FLight Light = GetLightAt(0);
        const float FarShadow = bReceiveShadow ? SunFarShadowMask(ScreenPos) : 1.0;

        // Pixels facing away from the sun are self-shadowed, so they skip the cascade lookup; SunL feeds the BRDF too.
        float3 SunL    = normalize(Light.Direction.xyz);
        float  SunNdotL = dot(N, SunL);
        const float3 SunMask = LightFunctionMask(Light, SunL, WorldPosition);

        // Backlit foliage, which the lit path below early-outs on.
        if (bFoliage && SunNdotL <= 0.0)
        {
            const float BackShadow = SunTransmissionShadow(Light, WorldPosition, bReceiveShadow) * G.SelfShadow * FarShadow;
            Lo += EvaluateTransmission(Ctx, SunL, ComputeLightRadiance(Light, SunL, 0.0),
                                       G.Transmission, G.Thickness) * (BackShadow * SunMask);
        }

        if (SunNdotL > 0.0)
        {
            // The parallax self-occlusion only ever traced against the sun, so it applies here and not to the whole loop.
            float Shadow = ComputeShadowFactor(Light, WorldPosition, N, ViewPosition, ScreenPos, bReceiveShadow);
            Shadow *= G.SelfShadow * FarShadow;

            // A leaf lit from the front still passes some light forward at grazing view angles.
            if (bFoliage)
            {
                const float FrontShadow = SunTransmissionShadow(Light, WorldPosition, bReceiveShadow) * G.SelfShadow * FarShadow;
                Lo += EvaluateTransmission(Ctx, SunL, ComputeLightRadiance(Light, SunL, 0.0),
                                           G.Transmission, G.Thickness) * (FrontShadow * SunMask);
            }

            if (Shadow > 0.0)
            {
#ifdef TRANSLUCENT
                float3 SunDiffuse, SunSpec;
                EvaluateLightContributionSplit(Light, Ctx, SunL, 0.0, SunDiffuse, SunSpec);
                Lo     += SunDiffuse * (Shadow * SunMask);
                LoSpec += SunSpec * (Shadow * SunMask);
#else
                float3 SunBase = EvaluateLightContribution(Light, Ctx, SunL, 0.0);

                if (Coat > 0.0)
                {
                    // The coat reflects some light before it reaches the base, so the base loses exactly what the coat took.
                    float CoatF;
                    const float CoatSpec = EvaluateClearcoatLobe(N, V, SunL, G.ClearcoatRoughness, CoatF);
                    SunBase *= (1.0 - Coat * CoatF);
                    LoCoat  += ComputeLightRadiance(Light, SunL, 0.0) * (CoatSpec * Coat) * (Shadow * SunMask);
                }

                Lo += SunBase * (Shadow * SunMask);
#endif
            }
        }
    }

    const float2 ShadowNoiseDir = LocalShadowNoiseDir(ScreenPos);

    uint    NumLights   = GetNumLights();
    FLight* LightArray  = LightData().Lights.At(0);
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
                float3 Radiance = GetLightColor(Light).rgb * Light.Intensity * Attenuation
                                * LightFunctionMask(Light, L, WorldPosition);

                // An area light lights diffuse and specular from different directions; every other light uses L for both.
                float3       DiffuseL         = L;
                float3       SpecularL        = L;
                float3       SpecularRadiance = Radiance;
                const bool   bArea            = HasFlag(Light.Flags, ELightFlags::Area);
                if (bArea)
                {
                    const FAreaLightTerms Terms = EvaluateAreaLight(Light, Radiance, WorldPosition, N, V,
                                                                    max(Ctx.Roughness, GetLightMinRoughness(Light)));
                    DiffuseL         = Terms.DiffuseL;
                    Radiance         = Terms.DiffuseRadiance;
                    SpecularL        = Terms.SpecularL;
                    SpecularRadiance = Terms.SpecularRadiance;
                }

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
                        Lo += EvaluateTransmission(Ctx, DiffuseL, Radiance, G.Transmission, G.Thickness) * Shadow;
                    }

                    float3 LightDiffuse, LightSpec;
                    EvaluateRadianceContributionSplit(Light, Ctx, DiffuseL, Radiance, LightDiffuse, LightSpec);
                    if (bArea)
                    {
                        float3 Unused;
                        EvaluateRadianceContributionSplit(Light, Ctx, SpecularL, SpecularRadiance, Unused, LightSpec);
                    }

#ifdef TRANSLUCENT
                    Lo     += LightDiffuse * Shadow;
                    LoSpec += LightSpec * Shadow;
#else
                    float3 LightBase = LightDiffuse + LightSpec;

                    if (Coat > 0.0)
                    {
                        float CoatF;
                        const float CoatSpec = EvaluateClearcoatLobe(N, V, SpecularL, G.ClearcoatRoughness, CoatF);
                        LightBase *= (1.0 - Coat * CoatF);
                        LoCoat    += SpecularRadiance * (CoatSpec * Coat) * Shadow;
                    }

                    Lo += LightBase * Shadow;
#endif
                }
            }
        }
    }

    // Translucent skips GTAO, which comes from opaque depth and holds what is behind the glass.
    float GTAO = 1.0;
#ifndef TRANSLUCENT
    // The compute lane tests at runtime, since deferred lighting does not specialize on GTAO and the fetch is one texel.
#ifdef SHADING_COMPUTE
    if (Scene().GTAOSettings.AOTexture.IsValid())
#else
    if (SPEC_ENABLE_GTAO != 0 && Scene().GTAOSettings.AOTexture.IsValid())
#endif
    {
        // Explicit LOD, since the deferred lighting lane is compute and has no implicit derivatives.
        float2 ScreenUV = ScreenPos / float2(Scene().ScreenSize.xy);
        GTAO = Scene().GTAOSettings.AOTexture.SampleLevel(Samplers::LinearClamp, ScreenUV, 0.0).r;
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
        Ambient = IBLDiffuse;
        LoSpec += IBLSpec;
#else
        float3 IBLAmbient = EvaluateIBLAmbient(WorldPosition, N, V, NdotV_IBL, DFG_IBL, Albedo, F0, Roughness, Metallic, AmbientAO);
        Ambient = IBLAmbient;
#endif
    }
    else
    {
        // Without an environment a SkyLight alone fills a flat diffuse ambient; metals carry no diffuse.
        float3 DiffuseColor = Albedo * (1.0 - Metallic);
        Ambient = GetAmbientLightColor() * GetAmbientLightIntensity() * DiffuseColor
                * AOMultiBounce(AmbientAO, DiffuseColor);
    }
    // The coat reflects the environment too, which is most of the read on car paint.
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

// Debug-only surface identity that never reaches the GBuffer; the deferred pass rebuilds it from the VisBuffer.
struct FDebugSurfaceInfo
{
    float2 UV;
    float3 GeometricNormalWS;   // interpolated vertex normal, before the normal map
    uint   MeshletIndex;
    uint   MaterialIndex;

    // Only the deferred lane fills this; the others pass kNoEdgeDistance and draw no wire.
    float  EdgeDistancePixels;
};

static const float kNoEdgeDistance = 1e30;

// Borrowed GBuffer channels paint violet where not stored for the model and dark teal where the surface is not clearcoat.
static const float3 kDebugNotStored = float3(0.35, 0.0, 0.5);
static const float3 kDebugNoCoat    = float3(0.0, 0.06, 0.08);

// Cyan is the hue least likely to sit in a lit scene already.
static const float3 kWireframeColor      = float3(0.15, 1.0, 1.0);
static const float  kWireframeWidthPixels = 1.25;

// Callers gate on SPEC_ENABLE_DEBUG_VIEWS so shipping pipelines strip the whole switch.
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
    case DEBUG_MODE_SUN_FAR_SHADOW:
    {
        // Terrain and distance-field occlusion past the cascades. White where nothing was traced.
        const float Far = SunFarShadowMask(ScreenPos);
        Color = float3(Far, Far, Far);
        break;
    }
    case DEBUG_MODE_GTAO:
    {
        // White where GTAO is off for this view.
        float DbgAO = 1.0;
        if (Scene().GTAOSettings.AOTexture.IsValid())
        {
            float2 DbgUV = ScreenPos / float2(Scene().ScreenSize.xy);
            DbgAO = Scene().GTAOSettings.AOTexture.SampleLevel(Samplers::LinearClamp, DbgUV, 0.0).r;
        }
        Color = float3(DbgAO, DbgAO, DbgAO);
        break;
    }
    case DEBUG_MODE_MATERIAL_ID:
        Color = MeshletDebugColor(Info.MaterialIndex);
        break;
    case DEBUG_MODE_SPECULAR:
        // Not stored for clearcoat, whose coat drives its own fixed F0.
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? kDebugNotStored
                    : float3(G.Specular, G.Specular, G.Specular);
        break;
    case DEBUG_MODE_SHADING_MODEL:
        switch (G.ShadingModel)
        {
            case ShadingModel_Default:   Color = float3(0.25, 0.35, 0.55); break;
            case ShadingModel_Unlit:     Color = float3(0.95, 0.55, 0.10); break;
            case ShadingModel_Clearcoat: Color = float3(0.10, 0.85, 0.85); break;
            default:                     Color = float3(1.0, 0.0, 1.0);    break;   // unmapped model
        }
        break;
    case DEBUG_MODE_CLEARCOAT:
        // Uncoated surfaces get a hue so they cannot be mistaken for a coat dialed to zero.
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
        // The material's own shadow term, not the shadow map; borrowed away by clearcoat like Specular.
        Color = (G.ShadingModel == ShadingModel_Clearcoat)
                    ? kDebugNotStored
                    : float3(G.SelfShadow, G.SelfShadow, G.SelfShadow);
        break;
    case DEBUG_MODE_WIREFRAME_OVERLAY:
    {
        // The only compositing mode, so Color arrives lit and the wire draws over it.
        const float Half = 0.5 * kWireframeWidthPixels;
        const float Wire = 1.0 - smoothstep(Half, Half + 1.0, Info.EdgeDistancePixels);
        Color = lerp(Color, kWireframeColor, Wire);
        break;
    }
    case DEBUG_MODE_PROBE_INFLUENCE:
    {
        // Saturation separates status from coverage; red is no probes, blue no array, yellow none baked, magenta outside all.
        FProbeDebugInfo Dbg = DebugReflectionProbes(WorldPosition);
        switch (Dbg.Status)
        {
            case kProbeStatusNoProbes:   Color = float3(1.0, 0.0, 0.0); break;
            case kProbeStatusNoArray:    Color = float3(0.0, 0.3, 1.0); break;
            case kProbeStatusNoneBaked:  Color = float3(1.0, 0.85, 0.0); break;
            case kProbeStatusOutsideAll: Color = float3(1.0, 0.0, 1.0); break;
            default:
                // MeshletDebugColor would hash slice 0 to black, and the floor keeps a barely-reaching probe's hue.
                Color = ProbeDebugColor((uint)Dbg.DominantSlice) * (0.25 + 0.75 * Dbg.TotalWeight);
                break;
        }
        break;
    }
    case DEBUG_MODE_PROBE_RADIANCE:
    {
        // Divided by coverage so a half-covered pixel shows the capture at full brightness.
        FProbeSample DbgSample = SampleReflectionProbes(WorldPosition, reflect(-V, N), 0.0);
        Color = (DbgSample.Weight > 1e-3)
                    ? DbgSample.Radiance / DbgSample.Weight
                    : float3(0.0);
        break;
    }
    }

    return Color;
}

// Forward lane for terrain and translucency; opaque meshes run the two halves in separate deferred passes.
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

    uint DebugMode = Scene().CullData.DebugMode;
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && DebugMode != DEBUG_MODE_NONE)
    {
#ifdef TRANSLUCENT
        Out.Reflection = float3(0.0);
#endif
        FDebugSurfaceInfo Info;
        Info.UV                = Input.UV;
        Info.GeometricNormalWS = Input.NormalWS.xyz;
        Info.MeshletIndex      = Input.MeshletIndex;
        Info.MaterialIndex     = Input.MaterialIndex;
        // No VisBuffer triangle in the forward lane, so no wire.
        Info.EdgeDistancePixels = kNoEdgeDistance;

        Out.Color = ApplyDebugView(DebugMode, Out.Color, G, Info, WorldPosition, ViewPosition, Input.Position.xy);
    }

    return Out;
}

#endif // !SHADING_COMPUTE
