#pragma once
#include "SceneGlobals.slang"
#include "CloudCommon.slang"

// The sun's cascade count; the atlas always holds NUM_CASCADES tiles but a light may use fewer.
int ActiveCascadeCount() { return clamp(int(LightData().ShadowParams2.z), 1, NUM_CASCADES); }

// The far cascade, when present, is the last active one and holds only far casters.
bool HasFarCascade() { return LightData().ShadowParams2.w > 0.5; }
int NearCascadeCount() { return max(ActiveCascadeCount() - (HasFarCascade() ? 1 : 0), 1); }

// A shadow map read through the comparison sampler, or point sampled for its raw depth.
struct FShadowTex
{
    FTexture2D Tex;

    float  SampleCmp(float2 UV, float Ref)          { return Tex.SampleCmp(Samplers::Shadow, UV, Ref); }
    float  SampleCmpLevelZero(float2 UV, float Ref) { return Tex.SampleCmpLevelZero(Samplers::Shadow, UV, Ref); }
    float4 SampleLevel(float2 UV, float Lod)        { return Tex.SampleLevel(Samplers::PointClamp, UV, Lod); }
    void   GetDimensions(out uint W, out uint H)    { Tex.GetDimensions(W, H); }
};

FShadowTex ShadowCascade() { return { GetSceneRoot().ShadowCascadeTexture }; }
FShadowTex ShadowAtlas()   { return { GetSceneRoot().ShadowAtlasTexture }; }

struct FCubemapCoord
{
    int Face;
    float2 UV;
};

// Face of the point-light shadow cube a direction falls in, and its UV within that face.
FCubemapCoord DirectionToCubemapCoord(float3 Dir)
{
    FCubemapCoord Result;

    float3 AbsDir = abs(Dir);

    // Exclusive picks, so a two-axis tie selects exactly one face.
    float IsXMax = (AbsDir.x >= AbsDir.y && AbsDir.x >= AbsDir.z) ? 1.0 : 0.0;
    float IsYMax = (IsXMax == 0.0 && AbsDir.y >= AbsDir.z) ? 1.0 : 0.0;
    float IsZMax = 1.0 - IsXMax - IsYMax;

    if (IsXMax > 0.0)
    {
        Result.Face = Dir.x >= 0.0 ? 0 : 1;
    }
    else if (IsYMax > 0.0)
    {
        Result.Face = Dir.y >= 0.0 ? 2 : 3;
    }
    else
    {
        Result.Face = Dir.z >= 0.0 ? 4 : 5;
    }

    float2 UVX = float2( sign(Dir.x) * Dir.z,  Dir.y)                / AbsDir.x;
    float2 UVY = float2(-Dir.x,                -sign(Dir.y) * Dir.z) / AbsDir.y;
    float2 UVZ = float2(-sign(Dir.z) * Dir.x,   Dir.y)                / AbsDir.z;

    Result.UV = IsXMax * UVX + IsYMax * UVY + IsZMax * UVZ;
    Result.UV = Result.UV * 0.5 + 0.5;

    return Result;
}

// Only the shadowed directional light exists, and the cloud map is built along its direction.
float SampleDirectionalCloudShadow(float3 FragmentPos)
{
    return CloudShadow_Sample(Scene().FogCloudShadow, FragmentPos, Scene().FogCloudShadowCenter, Scene().FogCloudShadowExtent);
}

// Cascades nest by distance, so FirstCascade carries a receding march's last hit and skips impossible ones.
float SampleSunCascadeFrom(FLight Light, float3 WorldPos, inout int FirstCascade)
{
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }

    // Unrolling holds all four cascade matrices live at once, and a volumetric march calls this per step.
    [loop]
    for (int c = FirstCascade; c < ActiveCascadeCount(); ++c)
    {
        float4x4 VP = GetShadowViewProjection(Light.ShadowDataIndex, c);
        float4   SC = mul(VP, float4(WorldPos, 1.0));
        float3   P  = SC.xyz / max(SC.w, 1e-6);
        float2   UV = P.xy * 0.5 + 0.5;

        if (all(UV >= 0.0) && all(UV <= 1.0) && P.z >= 0.0 && P.z <= 1.0)
        {
            FLightShadow Tile = GetShadowTile(Light.ShadowDataIndex, c);
            float2 AUV = UV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
            FirstCascade = c;
            return ShadowCascade().SampleCmpLevelZero(AUV, P.z - 0.0005);
        }
    }
    return 1.0;
}

// Containment is independent of the view-Z sign convention, unlike the lit pass's split-distance test.
float SampleSunCascadeByContainment(FLight Light, float3 WorldPos)
{
    int FirstCascade = 0;
    return SampleSunCascadeFrom(Light, WorldPos, FirstCascade);
}

// Traces in view space off the opaque depth buffer, so no derivatives and either lane can call it.
float ContactShadow(float3 ViewPos, float3 ViewN, float3 ViewL, float Jitter, float Reach, uint Steps)
{
    const FTexture2D SceneDepth = Scene().SceneDepth;
    if (Reach <= 0.0 || Steps == 0u || !SceneDepth.IsValid())
    {
        return 1.0;
    }

    const float4x4 InvProj    = GetInverseCameraProjection();
    const float2   ScreenSize = float2(GetScreenSize());
    const float3   Step       = ViewL * (Reach / float(Steps));
    const float    Lift    = Reach / float(Steps);

    // Half-step jittered start plus a normal offset, or a grazing surface reports its own ray as occluding.
    float3 RayPos = ViewPos + ViewN * Lift + Step * (0.5 + Jitter);

    // A hit farther behind the ray than this is a separate surface, not an occluder in contact.
    const float Thickness = Reach;

    [loop]
    for (uint i = 0u; i < Steps; ++i)
    {
        const float4 Clip = mul(GetCameraProjection(), float4(RayPos, 1.0));
        if (Clip.w <= 0.0)
        {
            break;
        }

        const float2 UV = (Clip.xy / Clip.w) * 0.5 + 0.5;
        if (any(saturate(UV) != UV))
        {
            break;   // left the depth buffer, and off-screen occluders are not knowable here
        }

        const float Raw = SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
        if (Raw > 0.0)
        {
            // Rebuilt where the depth was measured, since the ray's own UV would lift a grazing receiver off its plane.
            const float2 TexelUV = (floor(UV * ScreenSize) + 0.5) / ScreenSize;
            const float3 SceneP  = ReconstructViewPos(TexelUV, Raw, InvProj);
            const float  Delta  = RayPos.z - SceneP.z;   // positive = the ray is behind the depth buffer

            // A real occluder in front of the ray stands higher off the receiver plane than the ray does.
            const float RayHeight   = dot(RayPos - ViewPos, ViewN);
            const float SceneHeight = dot(SceneP - ViewPos, ViewN);

            if (Delta > 0.0 && Delta < Thickness && SceneHeight > 0.5 * RayHeight)
            {
                // Fade out along the ray so the far end of the reach does not cut a hard line.
                return saturate(float(i) / float(Steps));
            }
        }

        RayPos += Step;
    }

    return 1.0;
}

// World-space gap between this point and the nearest occluder the cascade recorded toward the sun.
float SunOccluderDepthWS(FLight Light, float3 WorldPos)
{
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 0.0;
    }

    // Raw depth through the point sampler, so no derivatives and the compute lane can call it.
    [loop]
    for (int c = 0; c < ActiveCascadeCount(); ++c)
    {
        float4x4 VP = GetShadowViewProjection(Light.ShadowDataIndex, c);
        float4   SC = mul(VP, float4(WorldPos, 1.0));
        float3   P  = SC.xyz / max(SC.w, 1e-6);
        float2   UV = P.xy * 0.5 + 0.5;

        if (all(UV >= 0.0) && all(UV <= 1.0) && P.z >= 0.0 && P.z <= 1.0)
        {
            FLightShadow Tile     = GetShadowTile(Light.ShadowDataIndex, c);
            float2       AtlasUV  = UV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
            float        Occluder = ShadowCascade().SampleLevel(AtlasUV, 0.0).r;

            // A surface that is its own nearest occluder reads 0, which is the backlit-leaf case.
            return max(P.z - Occluder, 0.0) * LightData().CascadeDepthRanges[c];
        }
    }

    // Past the last cascade there is no occluder record, which reads the same as nothing in the way.
    return 0.0;
}

// ToLight points toward the light like FLight::Direction, and the travel direction would zero NdotL.
float ComputeShadowBias(uint LightFlags, float3 Normal, float3 ToLight, int CascadeIndex)
{
    float NdotL       = saturate(dot(Normal, ToLight));
    float SlopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0));

    if (HasFlag(LightFlags, ELightFlags::Directional))
    {
        // NormalOffsetWS handles slope acne, and ShadowParams.y is the per-light constant bias.
        return 0.00015 + 0.0003 * SlopeFactor + LightData().ShadowParams.y;
    }

    // Point and spot RefDepth is NDC z from the face VP, which clusters near 1.0 and needs a tiny bias.
    return 0.00005 + 0.0003 * SlopeFactor;
}

// Normal-offset bias (Holbert, GDC 2011), about one texel along the normal and only at grazing angles.
float3 NormalOffsetWS(float3 Normal, float3 ToLight, int CascadeIndex)
{
    float NdotL       = saturate(dot(Normal, ToLight));
    float OffsetScale = saturate(1.0 - NdotL);

    // One shadow texel in world space, an ortho width of 2*Radius over the cascade resolution.
    float Radius     = LightData().CascadeRadii[CascadeIndex];
    float Resolution = max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    float TexelSize  = (2.0 * Radius) / Resolution;

    // About sqrt(2), one diagonal texel, the minimum that clears quantization acne without peter-panning.
    return Normal * (TexelSize * 1.4 * OffsetScale * LightData().ShadowParams.x);
}

// tan(80 degrees). Past it the plane estimate is untrustworthy, and a normal-mapped shading normal can swing it.
static const float kMaxReceiverSlope = 5.67;

// Depth change per atlas UV across the receiver's plane, so a tap off the kernel center compares against the surface under it.
float2 ReceiverPlaneDepthSlope(float4x4 CascadeVP, float3 Normal, float3 ToLight, float2 AtlasUVScale)
{
    const float3 Tangent   = normalize(cross(Normal, abs(Normal.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    const float3 Bitangent = cross(Normal, Tangent);

    // Directions with w at 0, so the orthographic cascade's translation drops out.
    const float3 D1 = mul(CascadeVP, float4(Tangent, 0.0)).xyz;
    const float3 D2 = mul(CascadeVP, float4(Bitangent, 0.0)).xyz;
    const float2 U1 = D1.xy * 0.5 * AtlasUVScale;
    const float2 U2 = D2.xy * 0.5 * AtlasUVScale;

    const float Det = U1.x * U2.y - U2.x * U1.y;
    if (abs(Det) < 1e-12)
    {
        return float2(0.0, 0.0);
    }
    float2 Slope = float2(U2.y * D1.z - U1.y * D2.z, U1.x * D2.z - U2.x * D1.z) / Det;

    const float CosTheta = saturate(abs(dot(Normal, ToLight)));
    const float TanTheta = sqrt(max(1.0 - CosTheta * CosTheta, 0.0)) / max(CosTheta, 1e-4);
    Slope *= min(1.0, kMaxReceiverSlope / max(TanTheta, 1e-4));
    return Slope;
}

// Godot's default soft sun shadow, a few hardware-compare taps on a rotated Vogel disk with no blocker search.
float SampleSunCascadePCF(FLight Light, int CascadeIndex, float3 WorldPos, float3 Normal, float3 ToLight, float2 ScreenPos, out bool Inside)
{
    // Godot's normal bias, minus its along-light part so it slides the receiver sideways rather than toward the sun.
    float3 NormalOffset = NormalOffsetWS(Normal, ToLight, CascadeIndex);
    NormalOffset -= ToLight * dot(ToLight, NormalOffset);

    const float4x4 CascadeVP = GetShadowViewProjection(Light.ShadowDataIndex, CascadeIndex);
    const float4   Coord     = mul(CascadeVP, float4(WorldPos + NormalOffset, 1.0));
    const float3   Proj      = Coord.xyz / max(Coord.w, 1e-6);
    const float2   CascadeUV = Proj.xy * 0.5 + 0.5;

    Inside = all(CascadeUV >= 0.0) && all(CascadeUV <= 1.0) && Proj.z >= 0.0 && Proj.z <= 1.0;
    if (!Inside)
    {
        return 1.0;
    }

    const FLightShadow Tile     = GetShadowTile(Light.ShadowDataIndex, CascadeIndex);
    const float2       BaseUV   = CascadeUV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    const float2       TileMin  = Tile.AtlasUVOffset;
    const float2       TileMax  = Tile.AtlasUVOffset + Tile.AtlasUVScale;
    const float        RefDepth = Proj.z - ComputeShadowBias(Light.Flags, Normal, ToLight, CascadeIndex);

    // A far texel is meters wide, so one bilinear compare already filters more than the near kernel would.
    const int Taps = CascadeIndex >= NearCascadeCount() ? 1 : clamp(int(LightData().ShadowParams2.y), 1, SHADOW_MAX_SAMPLE_COUNT);
    if (Taps == 1)
    {
        return ShadowCascade().SampleCmpLevelZero(BaseUV, RefDepth);
    }

    // Set in cascade-0 texels and held at that world size, so far splits do not blur wider. A cascade spans 2 * radius.
    const float  FilterTexels = LightData().ShadowParams.z;
    const float  RadiusUV0    = FilterTexels / max(LightData().CascadeResolutions[0], 1.0);
    const float2 RadiusUV     = RadiusUV0 * (LightData().CascadeRadii[0] / max(LightData().CascadeRadii[CascadeIndex], 1e-4)) * Tile.AtlasUVScale;

    // Outer taps of a wide kernel need the plane's slope or they stripe flat ground.
    const float2 DepthSlope = ReceiverPlaneDepthSlope(CascadeVP, Normal, ToLight, Tile.AtlasUVScale);

    // Each tap's bilinear compare reads texel centers up to a texel away, so the plane's rise over one texel is owed too.
    const float2 TexelUV  = Tile.AtlasUVScale / max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    const float  TapDepth = RefDepth - dot(abs(DepthSlope), TexelUV);

    float2 Dir;
    sincos(InterleavedGradientNoise(ScreenPos) * TWO_PI, Dir.y, Dir.x);
    const float2 GoldenStep = float2(cos(kVogelGoldenAngle), sin(kVogelGoldenAngle));

    float Shadow = 0.0;
    [loop]
    for (int i = 0; i < Taps; ++i)
    {
        const float2 TapUV = Dir * sqrt((float(i) + 0.5) / float(Taps)) * RadiusUV;
        Dir = RotateVogel(Dir, GoldenStep);
        Shadow += ShadowCascade().SampleCmpLevelZero(clamp(BaseUV + TapUV, TileMin, TileMax), TapDepth + dot(DepthSlope, TapUV));
    }
    return Shadow / float(Taps);
}

// No derivatives anywhere below, so the compute lighting pass and the forward passes share this one path.
float ComputeDirectionalCascadeShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition, float2 ScreenPos)
{
    // A sun with bCastShadows off leaves ShadowDataIndex at INDEX_NONE.
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }

    const float4 CascadeSplits = LightData().CascadeSplits;
    const int NumActive = ActiveCascadeCount();
    int CascadeIndex = 0;
    for (int i = 0; i < NumActive; ++i)
    {
        if (ViewPosition.z > CascadeSplits[i])
        {
            CascadeIndex = i + 1;
        }
    }
    if (CascadeIndex >= NumActive)
    {
        return 1.0;
    }

    // Toward the light, matching the SunL the BRDF uses.
    const float3 ToLight = normalize(Light.Direction);

    bool InsideA;
    const float ShadowA = SampleSunCascadePCF(Light, CascadeIndex, FragmentPos, Normal, ToLight, ScreenPos, InsideA);

    const float CascadeNear = (CascadeIndex == 0) ? 0.0 : CascadeSplits[CascadeIndex - 1];
    const float CascadeLen  = max(CascadeSplits[CascadeIndex] - CascadeNear, 1e-4);
    const float LocalT      = saturate((ViewPosition.z - CascadeNear) / CascadeLen);

    // Pixels in the blend band near a split pay for a second cascade. Near shadows fade out into the far cascade over the distance fade.
    const bool  bIntoFar      = HasFarCascade() && CascadeIndex + 1 == NearCascadeCount();
    const float BlendFraction = bIntoFar ? max(LightData().ShadowParams.w, LightData().ShadowParams2.x) : LightData().ShadowParams.w;
    if (BlendFraction > 0.0 && CascadeIndex + 1 < NumActive && LocalT > 1.0 - BlendFraction)
    {
        bool InsideB;
        const float ShadowB = SampleSunCascadePCF(Light, CascadeIndex + 1, FragmentPos, Normal, ToLight, ScreenPos, InsideB);
        if (InsideB)
        {
            return lerp(ShadowA, ShadowB, saturate((LocalT - (1.0 - BlendFraction)) / BlendFraction));
        }
    }

    // Shadows dissolve over the outer slice of the last cascade instead of popping off at max distance.
    if (CascadeIndex + 1 == NumActive)
    {
        const float FadeStart = saturate(1.0 - LightData().ShadowParams2.x);
        if (LocalT > FadeStart)
        {
            return lerp(ShadowA, 1.0, saturate((LocalT - FadeStart) / max(1.0 - FadeStart, 1e-4)));
        }
    }

    return ShadowA;
}

float ComputeDirectionalShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition, float2 ScreenPos)
{
    return ComputeDirectionalCascadeShadow(Light, FragmentPos, Normal, ViewPosition, ScreenPos)
         * SampleDirectionalCloudShadow(FragmentPos);
}
