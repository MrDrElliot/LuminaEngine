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

// A spot on a roof edge or overhang that drips for a while, its drops falling to the surface FallHeight below.
struct FRainDrip
{
    float3 Origin;
    float  SpawnTime;
    float  FallHeight;
    uint   PackedLightRG;
    uint   PackedLightB;
    uint   Seed;
};

// Each drip site keeps a few drops in flight, every one drawn as a streak and then a ring where it lands.
static const uint  kDripDropsInFlight = 3u;
static const uint  kDripVertices      = kDripDropsInFlight * 2u * 6u;
static const float kDripSiteLifetime  = 4.0;

// Each splash draws two rings on the surface, a puff of mist and a few droplets thrown up from it.
static const uint  kSplashDroplets      = 6u;
static const uint  kSplashFirstDroplet  = 3u;
static const uint  kSplashVertices      = (kSplashFirstDroplet + kSplashDroplets) * 6u;
static const float kSplashLifetime      = 0.3;
// Spray hangs in the air a while after the drop that threw it has gone.
static const float kSplashMistLifetime  = 0.9;

static const float2 kRainQuadCorners[6] =
{
    float2(-1.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(-1.0, 0.0), float2(1.0, 1.0), float2(-1.0, 1.0),
};

struct FRainDrawArgs
{
    GPUSpan<uint4>       Visible;
    GPUSpan<FRainSplash> Splashes;
    GPUSpan<FRainDrip>   Drips;
    float                Exposure;
    float                SoftFadeDistance;
    // The scene as it stood before the weather drew, which drops refract; invalid when refraction is off.
    FTexture2D           SceneColor;
    uint                 _Pad0;
};

FRainDrawArgs* RainArgs() { return GetPassArgs<FRainDrawArgs>(); }

static const uint kRainKindStreak = 0u;
static const uint kRainKindRing   = 1u;
static const uint kRainKindCrown  = 2u;
static const uint kRainKindFlake  = 3u;
static const uint kRainKindMist   = 4u;

struct FRainVertexOutput
{
    float4                  Position  : SV_Position;
    float3                  Color     : RAIN_COLOR;
    float2                  Coord     : RAIN_COORD;
    float                   Alpha     : RAIN_ALPHA;
    float                   ViewDepth : RAIN_DEPTH;
    // xy = the ribbon's across direction in pixels as wide as it draws, z = refraction strength, w = rim reflection.
    nointerpolation float4  Lens      : RAIN_LENS;
    // Tint the light passing through the drop takes on.
    nointerpolation float3  Transmit  : RAIN_TRANSMIT;
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
    Out.Lens      = float4(0.0, 0.0, 0.0, 0.0);
    Out.Transmit  = float3(1.0, 1.0, 1.0);
    Out.Kind      = kRainKindStreak;
    return Out;
}

// A soft disc lying on the surface or facing the camera, which rings, mist and flakes share.
FRainVertexOutput ExpandQuad(float3 Center, float3 AxisX, float3 AxisY, float2 Corner, float3 Color, float Alpha, uint Kind)
{
    const float2 Quad  = float2(Corner.x, Corner.y * 2.0 - 1.0);
    const float3 World = Center + AxisX * Quad.x + AxisY * Quad.y;

    FRainVertexOutput Out = CulledRainVertex();
    Out.Position  = WorldToClip(World);
    Out.Color     = Color;
    Out.Coord     = Quad * 0.5 + 0.5;
    Out.Alpha     = Alpha;
    Out.ViewDepth = Out.Position.w;
    Out.Kind      = Kind;
    return Out;
}

// The water color one drop carries, picked between the two tints by its own seed.
float3 RainWaterTint(FWeatherData W, uint Seed)
{
    const float Pick = float(PCGHash(Seed ^ 0xB5297A4Du) >> 8u) * (1.0 / 16777216.0);
    return lerp(W.RainTint.rgb, W.RainTint2.rgb, Pick * W.RainTint2.w);
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

    FRainVertexOutput Out = CulledRainVertex();
    Out.Position  = Clip;
    Out.Color     = Color;
    Out.Coord     = Corner;
    Out.Alpha     = Alpha * (WidthPx / DrawPx);
    Out.ViewDepth = Clip.w;
    Out.Lens.xy   = Across * DrawPx;
    Out.Kind      = kRainKindStreak;
    return Out;
}
