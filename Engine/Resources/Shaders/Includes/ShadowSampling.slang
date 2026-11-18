#pragma once
#include "SceneGlobals.slang"
#include "CloudCommon.slang"

// The sun's cascade count; the atlas always holds NUM_CASCADES tiles but a light may use fewer.
int ActiveCascadeCount() { return clamp(int(LightData().SunShadow.ActiveCascadeCount), 1, NUM_CASCADES); }

// The far cascade, when present, is the last active one and holds only far casters.
bool HasFarCascade() { return LightData().SunShadow.bFarCascade != 0u; }
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

// Projects WorldPos into one sun cascade, false when it lands outside that cascade's volume.
bool ProjectToSunCascade(int ShadowDataIndex, int Cascade, float3 WorldPos, out float3 Proj, out float2 AtlasUV)
{
    const float4 Coord = mul(GetShadowViewProjection(ShadowDataIndex, Cascade), float4(WorldPos, 1.0));
    Proj    = Coord.xyz / max(Coord.w, 1e-6);
    AtlasUV = float2(0.0, 0.0);

    const float2 UV = Proj.xy * 0.5 + 0.5;

    if (!(all(UV >= 0.0) && all(UV <= 1.0) && Proj.z >= 0.0 && Proj.z <= 1.0))
    {
        return false;
    }

    const FLightShadow Tile = GetShadowTile(ShadowDataIndex, Cascade);
    AtlasUV = UV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    return true;
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
        float3 P;
        float2 AtlasUV;
        if (ProjectToSunCascade(Light.ShadowDataIndex, c, WorldPos, P, AtlasUV))
        {
            FirstCascade = c;
            return ShadowCascade().SampleCmpLevelZero(AtlasUV, P.z - 0.0005);
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

// Screen pixels a view-space length spans at this depth.
float ContactReachPixels(float Reach, float ViewZ, float ScreenHeight)
{
    return Reach * abs(GetCameraProjection()[1][1]) * 0.5 * ScreenHeight / max(abs(ViewZ), 1e-4);
}

static const uint kContactBayer[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };

// Every 4x4 block holds each of these start offsets once, so a 4x4 box over the result averages all sixteen.
float OrderedContactJitter(int2 Pixel)
{
    return (float(kContactBayer[(Pixel.y & 3) * 4 + (Pixel.x & 3)]) + 0.5) / 16.0;
}

// Depth an occluder in contact may extend behind its front face, as a fraction of view depth so precision loss far away is covered.
static const float kContactThicknessPerMeter = 0.02;

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
    const float    StepLength = Reach / float(Steps);
    const float3   Step       = ViewL * StepLength;

    // Jitter is a start offset in steps from 0 to 1, and the normal offset keeps a grazing surface off its own ray.
    float3 RayPos = ViewPos + ViewN * StepLength + Step * (0.5 + Jitter);

    // A hit farther behind the ray than this is a separate surface, so a background pixel beside a silhouette stays lit.
    const float Thickness = clamp(abs(ViewPos.z) * kContactThicknessPerMeter, StepLength, Reach);

    float Occlusion = 0.0;
    float PrevDelta = -StepLength;
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

            if (Delta > 0.0 && SceneHeight > 0.5 * RayHeight)
            {
                // Faded from where the ray met the surface between the two steps, so the fade is continuous rather than stepped.
                const float Crossing = PrevDelta <= 0.0 ? saturate(Delta / max(Delta - PrevDelta, 1e-6)) : 0.0;
                const float Along    = saturate((float(i) + 0.5 + Jitter - Crossing) / float(Steps));

                // Hits fade toward the thickness edge and along the ray, so neither limit cuts a hard line.
                Occlusion = max(Occlusion, saturate(2.0 * (1.0 - Delta / Thickness)) * (1.0 - Along));
                if (Delta * 2.0 <= Thickness)
                {
                    break;   // a full-strength hit, and later steps only fade weaker
                }
            }
            PrevDelta = Delta;
        }
        else
        {
            PrevDelta = -Reach;
        }

        RayPos += Step;
    }

    return 1.0 - Occlusion;
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
        float3 P;
        float2 AtlasUV;
        if (ProjectToSunCascade(Light.ShadowDataIndex, c, WorldPos, P, AtlasUV))
        {
            float Occluder = ShadowCascade().SampleLevel(AtlasUV, 0.0).r;

            // A surface that is its own nearest occluder reads 0, which is the backlit-leaf case.
            return max(P.z - Occluder, 0.0) * LightData().CascadeDepthRanges[c];
        }
    }

    // Past the last cascade there is no occluder record, which reads the same as nothing in the way.
    return 0.0;
}

static const float kSunShadowConstantBias = 0.00015;
// Point and spot RefDepth is NDC z from the face VP, which clusters near 1.0 and needs a tiny bias.
static const float kLocalShadowConstantBias = 0.00005;
static const float kShadowSlopeBias = 0.0003;

// ToLight points toward the light like FLight::Direction, and the travel direction would zero NdotL.
float ComputeSunShadowBias(float3 Normal, float3 ToLight)
{
    float NdotL       = saturate(dot(Normal, ToLight));
    float SlopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0));

    // NormalOffsetWS handles slope acne, and DepthBias is the per-light constant bias.
    return kSunShadowConstantBias + kShadowSlopeBias * SlopeFactor + LightData().SunShadow.DepthBias;
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
    return Normal * (TexelSize * 1.4 * OffsetScale * LightData().SunShadow.NormalBias);
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

// Castano's optimized PCF (2013), bilinear compares placed and weighted so their sum is a tent over whole texels with no noise.
struct FShadowPCF
{
    FShadowTex Tex;
    float2     UV;
    float2     TexelUV;
    float      RefDepth;
    float2     DepthSlope;
    float2     TileMin;
    float2     TileMax;

    // Spread above one widens the kernel past its size about UV, at the cost of exactness between taps.
    float Tap(float2 Corner, float2 OffsetTexels, float Spread)
    {
        const float2 P = UV + (Corner + OffsetTexels * TexelUV - UV) * Spread;
        return Tex.SampleCmpLevelZero(clamp(P, TileMin, TileMax), RefDepth + dot(DepthSlope, P - UV));
    }

    static void TentTaps(int Size, float S, out float4 W, out float4 O)
    {
        if (Size == 3)
        {
            W = float4(3.0 - 2.0 * S, 1.0 + 2.0 * S, 0.0, 0.0);
            O = float4((2.0 - S) / W.x - 1.0, S / W.y + 1.0, 0.0, 0.0);
        }
        else if (Size == 5)
        {
            W = float4(4.0 - 3.0 * S, 7.0, 1.0 + 3.0 * S, 0.0);
            O = float4((3.0 - 2.0 * S) / W.x - 2.0, (3.0 + S) / W.y, S / W.z + 2.0, 0.0);
        }
        else
        {
            W = float4(5.0 * S - 6.0, 11.0 * S - 28.0, -(11.0 * S + 17.0), -(5.0 * S + 1.0));
            O = float4((4.0 * S - 5.0) / W.x - 3.0, (4.0 * S - 16.0) / W.y - 1.0, -(7.0 * S + 5.0) / W.z + 1.0, -S / W.w + 3.0);
        }
    }

    // Size is the kernel's width in texels, 1, 3, 5 or 7.
    float Filter(int Size, float Spread)
    {
        if (Size <= 1)
        {
            return Tex.SampleCmpLevelZero(UV, RefDepth);
        }

        const float2 T      = UV / TexelUV;
        const float2 Snap   = floor(T + 0.5);
        const float2 Frac   = T + 0.5 - Snap;
        const float2 Corner = (Snap - 0.5) * TexelUV;

        float4 WU, OU, WV, OV;
        TentTaps(Size, Frac.x, WU, OU);
        TentTaps(Size, Frac.y, WV, OV);

        const int N = (Size + 1) / 2;
        float Sum = 0.0;
        [unroll]
        for (int j = 0; j < 4; ++j)
        {
            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                if (i < N && j < N)
                {
                    Sum += WU[i] * WV[j] * Tap(Corner, float2(OU[i], OV[j]), Spread);
                }
            }
        }
        return Sum / (dot(WU, float4(1.0, 1.0, 1.0, 1.0)) * dot(WV, float4(1.0, 1.0, 1.0, 1.0)));
    }

    // Blends the two kernel sizes around RadiusTexels, so contact hardening changes the width without a visible step.
    float FilterRadius(float RadiusTexels, int MaxSize)
    {
        const float MaxRadius = float(MaxSize) * 0.5;
        if (RadiusTexels >= MaxRadius)
        {
            return Filter(MaxSize, RadiusTexels / MaxRadius);
        }
        if (RadiusTexels <= 0.5)
        {
            return Filter(1, 1.0);
        }

        const float LowerRadius = floor(RadiusTexels - 0.5) + 0.5;
        const int   LowerSize   = int(LowerRadius * 2.0 + 0.5);
        const float Blend       = RadiusTexels - LowerRadius;
        const float Lower       = Filter(LowerSize, 1.0);
        return Blend > 0.0 ? lerp(Lower, Filter(LowerSize + 2, 1.0), Blend) : Lower;
    }
};

// The widest Castano kernel a tap budget affords, 16 taps for 7 texels, 9 for 5 and 4 for 3.
int ShadowKernelSizeForTaps(int Taps)
{
    return Taps >= 16 ? 7 : (Taps >= 8 ? 5 : 3);
}

float SampleSunCascadePCF(FLight Light, int CascadeIndex, float3 WorldPos, float3 Normal, float3 ToLight, out bool Inside)
{
    // Godot's normal bias, minus its along-light part so it slides the receiver sideways rather than toward the sun.
    float3 NormalOffset = NormalOffsetWS(Normal, ToLight, CascadeIndex);
    NormalOffset -= ToLight * dot(ToLight, NormalOffset);

    float3 Proj;
    float2 BaseUV;
    Inside = ProjectToSunCascade(Light.ShadowDataIndex, CascadeIndex, WorldPos + NormalOffset, Proj, BaseUV);
    if (!Inside)
    {
        return 1.0;
    }

    const FLightShadow Tile     = GetShadowTile(Light.ShadowDataIndex, CascadeIndex);
    const float2       TileMin  = Tile.AtlasUVOffset;
    const float2       TileMax  = Tile.AtlasUVOffset + Tile.AtlasUVScale;
    const float        RefDepth = Proj.z - ComputeSunShadowBias(Normal, ToLight);

    // A far texel is meters wide, so one bilinear compare already filters more than the near kernel would.
    const int Taps = CascadeIndex >= NearCascadeCount() ? 1 : clamp(int(LightData().SunShadow.FilterTaps), 1, SHADOW_MAX_SAMPLE_COUNT);
    if (Taps == 1)
    {
        return ShadowCascade().SampleCmpLevelZero(BaseUV, RefDepth);
    }

    // Set in cascade-0 texels and held at that world size, so far splits do not blur wider. A cascade spans 2 * radius.
    const float  RadiusUV0 = LightData().SunShadow.FilterRadiusTexels / max(LightData().CascadeResolutions[0], 1.0);
    const float2 RadiusUV  = RadiusUV0 * (LightData().CascadeRadii[0] / max(LightData().CascadeRadii[CascadeIndex], 1e-4)) * Tile.AtlasUVScale;

    // Outer taps of a wide kernel need the plane's slope or they stripe flat ground.
    const float4x4 CascadeVP  = GetShadowViewProjection(Light.ShadowDataIndex, CascadeIndex);
    const float2   DepthSlope = ReceiverPlaneDepthSlope(CascadeVP, Normal, ToLight, Tile.AtlasUVScale);

    // Each tap's bilinear compare reads texel centers up to a texel away, so the plane's rise over one texel is owed too.
    const float2 TexelUV  = Tile.AtlasUVScale / max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    const float  TapDepth = RefDepth - dot(abs(DepthSlope), TexelUV);

    const float2 StartDir   = float2(1.0, 0.0);
    const float2 GoldenStep = float2(cos(kVogelGoldenAngle), sin(kVogelGoldenAngle));

    // Contact hardening (PCSS, Fernando 2005), the sun's disk seen past the average blocker sets the kernel within the widest one.
    float2 KernelUV = RadiusUV;
    const int BlockerTaps = int(LightData().SunShadow.BlockerTaps);
    if (BlockerTaps > 0)
    {
        float  GapSum   = 0.0;
        float  Blockers = 0.0;
        float2 Dir      = StartDir;
        [loop]
        for (int i = 0; i < BlockerTaps; ++i)
        {
            const float2 TapUV  = Dir * sqrt((float(i) + 0.5) / float(BlockerTaps)) * RadiusUV;
            Dir = RotateVogel(Dir, GoldenStep);
            const float  TapRef = TapDepth + dot(DepthSlope, TapUV);
            const float  Depth  = ShadowCascade().SampleLevel(clamp(BaseUV + TapUV, TileMin, TileMax), 0.0).r;
            if (Depth < TapRef)
            {
                GapSum   += TapRef - Depth;
                Blockers += 1.0;
            }
        }

        // No blocker found keeps the widest kernel, so a misjudged receiver plane can only soften, never leak.
        if (Blockers > 0.0)
        {
            const float  PenumbraWS = GapSum / Blockers * LightData().CascadeDepthRanges[CascadeIndex] * LightData().SunShadow.TanSunRadius;
            const float2 UVPerMeter = Tile.AtlasUVScale / (2.0 * max(LightData().CascadeRadii[CascadeIndex], 1e-4));
            const float2 MinUV      = RadiusUV * (LightData().SunShadow.MinFilterRadiusTexels / LightData().SunShadow.FilterRadiusTexels);
            KernelUV = clamp(PenumbraWS * UVPerMeter, MinUV, RadiusUV);
        }
    }

    const FShadowPCF PCF = { ShadowCascade(), BaseUV, TexelUV, TapDepth, DepthSlope, TileMin, TileMax };
    return PCF.FilterRadius(KernelUV.x / TexelUV.x, ShadowKernelSizeForTaps(Taps));
}

// No derivatives anywhere below, so the compute lighting pass and the forward passes share this one path.
float ComputeDirectionalCascadeShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition)
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
    const float ShadowA = SampleSunCascadePCF(Light, CascadeIndex, FragmentPos, Normal, ToLight, InsideA);

    const float CascadeNear = (CascadeIndex == 0) ? 0.0 : CascadeSplits[CascadeIndex - 1];
    const float CascadeLen  = max(CascadeSplits[CascadeIndex] - CascadeNear, 1e-4);
    const float LocalT      = saturate((ViewPosition.z - CascadeNear) / CascadeLen);

    // Pixels in the blend band near a split pay for a second cascade. Near shadows fade out into the far cascade over the distance fade.
    const bool  bIntoFar      = HasFarCascade() && CascadeIndex + 1 == NearCascadeCount();
    const float BlendFraction = bIntoFar ? max(LightData().SunShadow.CascadeBlend, LightData().SunShadow.DistanceFade) : LightData().SunShadow.CascadeBlend;
    if (BlendFraction > 0.0 && CascadeIndex + 1 < NumActive && LocalT > 1.0 - BlendFraction)
    {
        bool InsideB;
        const float ShadowB = SampleSunCascadePCF(Light, CascadeIndex + 1, FragmentPos, Normal, ToLight, InsideB);
        if (InsideB)
        {
            return lerp(ShadowA, ShadowB, saturate((LocalT - (1.0 - BlendFraction)) / BlendFraction));
        }
    }

    // Shadows dissolve over the outer slice of the last cascade instead of popping off at max distance.
    if (CascadeIndex + 1 == NumActive)
    {
        const float FadeStart = saturate(1.0 - LightData().SunShadow.DistanceFade);
        if (LocalT > FadeStart)
        {
            return lerp(ShadowA, 1.0, saturate((LocalT - FadeStart) / max(1.0 - FadeStart, 1e-4)));
        }
    }

    return ShadowA;
}

float ComputeDirectionalShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition)
{
    return ComputeDirectionalCascadeShadow(Light, FragmentPos, Normal, ViewPosition)
         * SampleDirectionalCloudShadow(FragmentPos);
}
