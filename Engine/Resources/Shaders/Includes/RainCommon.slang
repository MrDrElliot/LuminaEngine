#pragma once
#include "SceneGlobals.slang"
#include "GlobalRHI.slang"

static const uint  kRainStreakVertices = 6u;
static const float kRainNearClip       = 0.35;
static const float kRainMinPixelWidth  = 1.25;

// One splash on a surface, which the draw ages analytically from its spawn time.
struct FRainSplash
{
    float3 Position;
    float  SpawnTime;
    uint   PackedNormal;
    uint   PackedLightRG;
    uint   PackedLightB;
    uint   Seed;
};

// Each splash draws a ring on the surface plus a few droplets thrown up from it.
static const uint kSplashDroplets      = 4u;
static const uint kSplashVertices      = (kSplashDroplets + 1u) * 6u;
static const float kSplashLifetime     = 0.3;

static const float2 kRainQuadCorners[6] =
{
    float2(-1.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(-1.0, 0.0), float2(1.0, 1.0), float2(-1.0, 1.0),
};

uint PackHalf2(float2 V)
{
    return f32tof16(V.x) | (f32tof16(V.y) << 16u);
}

float2 UnpackHalf2(uint P)
{
    return float2(f16tof32(P & 0xFFFFu), f16tof32(P >> 16u));
}

uint PackUnitVector(float3 N)
{
    const float2 E = clamp(N.xz * 0.5 + 0.5, 0.0, 1.0);
    return uint(E.x * 32767.0) | (uint(E.y * 32767.0) << 15u) | (N.y >= 0.0 ? 0x40000000u : 0u);
}

float3 UnpackUnitVector(uint P)
{
    const float2 E = float2(float(P & 0x7FFFu), float((P >> 15u) & 0x7FFFu)) / 32767.0 * 2.0 - 1.0;
    const float  Y = sqrt(saturate(1.0 - dot(E, E))) * ((P & 0x40000000u) != 0u ? 1.0 : -1.0);
    return normalize(float3(E.x, Y, E.y));
}

struct FRainDrawArgs
{
    GPUSpan<uint4>       Visible;
    GPUSpan<FRainSplash> Splashes;
    float                Exposure;
    float                SoftFadeDistance;
    uint                 _Pad0;
    uint                 _Pad1;
};

FRainDrawArgs* RainArgs() { return GetPassArgs<FRainDrawArgs>(); }

static const uint kRainKindStreak = 0u;
static const uint kRainKindRing   = 1u;
static const uint kRainKindCrown  = 2u;
static const uint kRainKindFlake  = 3u;

struct FRainVertexOutput
{
    float4                  Position  : SV_Position;
    float3                  Color     : RAIN_COLOR;
    float2                  Coord     : RAIN_COORD;
    float                   Alpha     : RAIN_ALPHA;
    float                   ViewDepth : RAIN_DEPTH;
    nointerpolation uint    Kind      : RAIN_KIND;
};

FRainVertexOutput CulledRainVertex()
{
    FRainVertexOutput Out;
    Out.Position  = float4(2.0, 2.0, 2.0, 1.0);
    Out.Color     = float3(0.0, 0.0, 0.0);
    Out.Coord     = float2(0.0, 0.0);
    Out.Alpha     = 0.0;
    Out.ViewDepth = 0.0;
    Out.Kind      = kRainKindStreak;
    return Out;
}

// A camera-facing ribbon from Head to Tail, never thinner than a pixel so distant streaks fade instead of shimmering.
FRainVertexOutput ExpandStreak(float3 Head, float3 Tail, float WidthWorld, float2 Corner, float3 Color, float Alpha)
{
    float4 ClipHead = WorldToClip(Head);
    float4 ClipTail = WorldToClip(Tail);
    if (ClipHead.w <= kRainNearClip * 0.5)
    {
        return CulledRainVertex();
    }
    if (ClipTail.w <= kRainNearClip * 0.5)
    {
        ClipTail = ClipHead;
    }

    const float2 HalfScreen = float2(GetScreenSize()) * 0.5;
    const float2 HeadPx = ClipHead.xy / ClipHead.w * HalfScreen;
    const float2 TailPx = ClipTail.xy / ClipTail.w * HalfScreen;
    const float2 Delta  = TailPx - HeadPx;
    const float  Length = length(Delta);
    const float2 Along  = Length > 1e-3 ? Delta / Length : float2(0.0, 1.0);
    const float2 Across = float2(-Along.y, Along.x);

    const float PixelsPerMeter = abs(GetCameraProjection()[1][1]) * HalfScreen.y / ClipHead.w;
    const float WidthPx  = WidthWorld * PixelsPerMeter;
    const float DrawPx   = max(WidthPx, kRainMinPixelWidth);

    float4 Clip = lerp(ClipHead, ClipTail, Corner.y);
    const float2 OffsetPx = Across * (Corner.x * DrawPx * 0.5) + Along * ((Corner.y * 2.0 - 1.0) * DrawPx * 0.5);
    Clip.xy += OffsetPx / HalfScreen * Clip.w;

    FRainVertexOutput Out;
    Out.Position  = Clip;
    Out.Color     = Color;
    Out.Coord     = Corner;
    Out.Alpha     = Alpha * (WidthPx / DrawPx);
    Out.ViewDepth = Clip.w;
    Out.Kind      = kRainKindStreak;
    return Out;
}
