#pragma once
#include "SceneGlobals.slang"
#include "CloudCommon.slang"
#include "VirtualShadow.slang"

// A shadow map read through the comparison sampler, or point sampled for its raw depth.
struct FShadowTex
{
    FTexture2D Tex;

    float  SampleCmp(float2 UV, float Ref)          { return Tex.SampleCmp(Samplers::Shadow, UV, Ref); }
    float  SampleCmpLevelZero(float2 UV, float Ref) { return Tex.SampleCmpLevelZero(Samplers::Shadow, UV, Ref); }
    float4 SampleLevel(float2 UV, float Lod)        { return Tex.SampleLevel(Samplers::PointClamp, UV, Lod); }
    void   GetDimensions(out uint W, out uint H)    { Tex.GetDimensions(W, H); }
};

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

// One compare at the finest resident shadow level, for fog, particles and other points with no surface to filter across.
float SampleSunShadowPoint(FLight Light, float3 WorldPos)
{
    return Light.ShadowDataIndex == INDEX_NONE ? 1.0 : SampleVirtualShadowPoint(WorldPos);
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

// World-space gap between this point and the nearest occluder toward the sun, 0 where nothing is recorded.
float SunOccluderDepthWS(FLight Light, float3 WorldPos)
{
    // A surface that is its own nearest occluder reads 0, which is the backlit-leaf case.
    return Light.ShadowDataIndex == INDEX_NONE ? 0.0 : VirtualShadowOccluderDistance(WorldPos);
}

// Point and spot RefDepth is NDC z from the face VP, which clusters near 1.0 and needs a tiny bias.
static const float kLocalShadowConstantBias = 0.00005;
static const float kShadowSlopeBias = 0.0003;

// tan(80 degrees). Past it the plane estimate is untrustworthy, and a normal-mapped shading normal can swing it.
static const float kMaxReceiverSlope = 5.67;

// Depth change per atlas UV across the receiver's plane, so a tap off the kernel center compares against the surface under it.
float2 ReceiverPlaneDepthSlope(float4x4 ViewProjection, float3 Normal, float3 ToLight, float2 UVScale)
{
    const float3 Tangent   = normalize(cross(Normal, abs(Normal.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    const float3 Bitangent = cross(Normal, Tangent);

    // Directions with w at 0, so the orthographic projection's translation drops out.
    const float3 D1 = mul(ViewProjection, float4(Tangent, 0.0)).xyz;
    const float3 D2 = mul(ViewProjection, float4(Bitangent, 0.0)).xyz;
    const float2 U1 = D1.xy * 0.5 * UVScale;
    const float2 U2 = D2.xy * 0.5 * UVScale;

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

// A bilinear box filter 2R+1 texels wide in one level, false when the page under the kernel's center is not resident.
bool SampleVirtualShadowPCF(uint Level, float3 WorldPos, float3 Normal, float3 ToLight, out float Shadow)
{
    Shadow = 1.0;
    const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);

    // About one diagonal texel along the normal at grazing angles, as the deferred sun lookup offsets.
    const float  OffsetScale = saturate(1.0 - saturate(dot(Normal, ToLight)));
    const float3 Receiver    = WorldPos + Normal * (Lv.TexelWorld * 1.4 * OffsetScale * LightData().SunShadow.NormalBias);
    float2 Texel;
    float  Depth;
    if (!ProjectToVirtualShadow(Lv, Receiver, Texel, Depth))
    {
        return false;
    }

    // Depth per virtual texel across the receiver's plane, so off-center taps compare against the surface under them.
    const float2 Slope    = ReceiverPlaneDepthSlope(Lv.ViewProjection, Normal, ToLight, float2(1.0, 1.0)) / float(VSM_VIRTUAL_SIZE);
    const float  BiasTexel = (Lv.TexelWorld + LightData().SunShadow.DepthBias) / max(Lv.DepthRange, 1.0e-4);
    const float  RefDepth = Depth - BiasTexel - dot(abs(Slope), float2(1.0, 1.0));

    // A virtual texel is already about a pixel, so one texel either side softens the edge enough.
    const int    Radius = clamp(int(LightData().SunShadow.FilterRadiusTexels + 0.5), 0, 1);
    const int2   Base   = int2(floor(Texel - 0.5));
    const float2 Frac   = Texel - 0.5 - float2(Base);

    // The kernel spans at most two pages a side, so their four lookups cover every tap.
    const int2 PageLo = (Base - Radius) >> int(VSM_PAGE_SHIFT);
    uint2 Origins[4];
    bool  bValid[4];
    [unroll]
    for (uint p = 0u; p < 4u; ++p)
    {
        bValid[p] = FindVirtualShadowPage(Level, Lv, PageLo + int2(p & 1u, p >> 1u), Origins[p]);
    }
    const int2 CenterPage = int2(Texel) >> int(VSM_PAGE_SHIFT);
    const int2 CenterSlot = CenterPage - PageLo;
    if (!bValid[CenterSlot.y * 2 + CenterSlot.x])
    {
        return false;
    }

    float Sum    = 0.0;
    float Weight = 0.0;
    for (int j = -Radius; j <= Radius + 1; ++j)
    {
        const float WY = j == -Radius ? 1.0 - Frac.y : (j == Radius + 1 ? Frac.y : 1.0);
        for (int i = -Radius; i <= Radius + 1; ++i)
        {
            const float WX = i == -Radius ? 1.0 - Frac.x : (i == Radius + 1 ? Frac.x : 1.0);
            const int2  T    = Base + int2(i, j);
            const int2  Slot = (T >> int(VSM_PAGE_SHIFT)) - PageLo;
            const uint  S    = uint(Slot.y * 2 + Slot.x);
            float Lit = 1.0;
            if (bValid[S])
            {
                const float Occluder = LoadVirtualShadowDepth(Origins[S] + (uint2(T) & (VSM_PAGE_SIZE - 1u)));
                Lit = Occluder >= RefDepth + dot(Slope, float2(T) + 0.5 - Texel) ? 1.0 : 0.0;
            }
            Sum    += WX * WY * Lit;
            Weight += WX * WY;
        }
    }
    Shadow = Sum / max(Weight, 1.0e-6);
    return true;
}

// The level a pixel's footprint picks, falling back to coarser ones where its own holds no page, and fading out by the shadowed range's end.
float ComputeDirectionalVirtualShadow(float3 FragmentPos, float3 Normal, float3 ToLight, float3 ViewPosition)
{
    const float Distance = length(FragmentPos - GetCameraPosition());
    const int   Wanted   = VirtualShadowLevelForDistance(Distance);
    const uint  Last     = LightData().SunShadow.LastLevel;
    if (Wanted > (int)Last)
    {
        return 1.0;
    }

    float Shadow = 1.0;
    const uint First = ClampVirtualShadowLevel(Wanted);
    uint Found = Last + 1u;
    [loop]
    for (uint Level = First; Level <= min(First + kVirtualShadowFallbackLevels, Last); ++Level)
    {
        if (SampleVirtualShadowPCF(Level, FragmentPos, Normal, ToLight, Shadow))
        {
            Found = Level;
            break;
        }
    }

    // Near a level's outer edge the next level fades in, so the step in texel size and caster detail never shows as a ring.
    const float Blend = Found == (uint)Wanted && Found < Last ? VirtualShadowLevelBlend(Distance) : 0.0;
    float Coarser;
    if (Blend > 0.0 && SampleVirtualShadowPCF(Found + 1u, FragmentPos, Normal, ToLight, Coarser))
    {
        Shadow = lerp(Shadow, Coarser, Blend);
    }

    const float ShadowFar = LightData().SunShadow.ShadowFar;
    const float FadeStart = ShadowFar * saturate(1.0 - LightData().SunShadow.DistanceFade);
    return lerp(Shadow, 1.0, saturate((ViewPosition.z - FadeStart) / max(ShadowFar - FadeStart, 1.0e-4)));
}

// No derivatives, so the compute lighting pass and the forward passes share this one path.
float ComputeDirectionalShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition)
{
    // A sun with bCastShadows off leaves ShadowDataIndex at INDEX_NONE.
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }
    return ComputeDirectionalVirtualShadow(FragmentPos, Normal, normalize(Light.Direction), ViewPosition)
         * SampleDirectionalCloudShadow(FragmentPos);
}
