#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

static const float3 kGravity = float3(0.0, -9.8, 0.0);
static const float  kSplashMistOpacity = 0.03;

// A drop landing on hard ground throws a crown of droplets and a puff of spray, on standing water it mostly rings.
[shader("vertex")]
FRainVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    if (!HasWeather() || !RainArgs().Splashes.Contains(InstanceIndex))
    {
        return CulledRainVertex();
    }

    const FRainSplash Splash = RainArgs().Splashes.Get(InstanceIndex);
    const FWeatherData W     = GetWeather();
    const float Age = W.Surface2.z - Splash.SpawnTime;
    if (Splash.Seed == 0u || Age < 0.0 || Age > kSplashMistLifetime)
    {
        return CulledRainVertex();
    }

    const float  T       = saturate(Age / kSplashLifetime);
    const float  Puddle  = saturate(UnpackHalf(Splash.PackedLightB >> 16u));
    const float3 Normal  = UnpackNormal(Splash.PackedNormal);
    const float2 LightRG = UnpackHalf2(Splash.PackedLightRG);
    const float3 Color   = float3(LightRG, UnpackHalf(Splash.PackedLightB & 0xFFFFu)) * RainWaterTint(W, Splash.Seed) * W.RainLook.y + W.RainEmissive.rgb;
    // Faded out right at the lens, where a centimeter ring would fill the screen, and past where a crown is resolvable.
    const float  Distance = length(Splash.Position - GetCameraPosition());
    const float  Opacity  = W.RainLook.z * saturate((Distance - 0.5) / 1.5);
    const float  DropletOpacity = Opacity * 0.3 * saturate((10.0 - Distance) / 6.0);

    uint Seed = Splash.Seed;
    const float Size    = W.Surface2.w * (0.6 + 0.8 * WeatherRandom(Seed));
    const uint  Element = VertexIndex / 6u;
    const float2 Corner = kRainQuadCorners[VertexIndex % 6u];

    const float3 Tangent   = normalize(abs(Normal.y) < 0.99 ? cross(Normal, float3(0.0, 1.0, 0.0)) : float3(1.0, 0.0, 0.0));
    const float3 Bitangent = cross(Normal, Tangent);

    if (Element < 2u)
    {
        // Standing water carries a wider ring and a second one chasing it, hard ground only a faint wet mark.
        const bool  bSecond = Element == 1u;
        const float Delay   = bSecond ? 0.25 : 0.0;
        const float RingT   = saturate((Age / kSplashLifetime - Delay) / (1.0 - Delay));
        const float Reach   = lerp(0.05, 0.11, Puddle) * (bSecond ? 1.35 : 1.0);
        const float Strength = bSecond ? Puddle * 0.6 : lerp(0.35, 0.6, Puddle);
        if (Age > kSplashLifetime || Strength <= 0.0 || (bSecond && Age / kSplashLifetime < Delay))
        {
            return CulledRainVertex();
        }
        const float Radius = Size * (0.01 + Reach * sqrt(RingT));
        return ExpandQuad(Splash.Position + Normal * 0.004, Tangent * Radius, Bitangent * Radius, Corner, Color,
                          Opacity * Strength * (1.0 - RingT) * (1.0 - RingT), kRainKindRing);
    }

    if (Element == 2u)
    {
        // Thousands of splashes overlap within a few meters, so only a quarter throw the wind-drifted spray and each puff is faint.
        const float MistT   = Age / kSplashMistLifetime;
        const float Heavy   = W.RainDrops.x * W.RainDrops.x;
        const bool  bThrows = (PCGHash(Splash.Seed ^ 0x68E31DA4u) & 3u) == 0u;
        const float Amount  = W.RainOptics.w * Heavy * (1.0 - Puddle * 0.7) * saturate((15.0 - Distance) / 6.0);
        if (!bThrows || Amount <= 0.0 || MistT >= 1.0)
        {
            return CulledRainVertex();
        }
        const float  Radius = Size * (0.04 + 0.1 * sqrt(MistT));
        const float3 Drift  = float3(W.Motion.x, 0.0, W.Motion.y) * (0.15 * Age) + Normal * (Radius * 0.5);
        const float  Fade   = smoothstep(0.0, 0.15, MistT) * (1.0 - MistT) * (1.0 - MistT);
        return ExpandQuad(Splash.Position + Drift, GetCameraRight() * Radius, GetCameraUp() * Radius, Corner, Color,
                          Opacity * Amount * kSplashMistOpacity * Fade, kRainKindMist);
    }

    if (Age > kSplashLifetime)
    {
        return CulledRainVertex();
    }

    for (uint Skip = kSplashFirstDroplet; Skip < Element; ++Skip)
    {
        WeatherRandom(Seed);
        WeatherRandom(Seed);
        WeatherRandom(Seed);
    }
    const float Angle  = WeatherRandom(Seed) * TWO_PI;
    // Water swallows most of the impact, so a puddle throws fewer and lower droplets than stone.
    const float Lift   = lerp(1.0, 0.6, Puddle);
    const float Up     = (1.1 + 1.1 * WeatherRandom(Seed)) * sqrt(Size) * Lift;
    const float Spread = (0.4 + 0.8 * WeatherRandom(Seed)) * sqrt(Size);
    if (Element - kSplashFirstDroplet >= uint(lerp(float(kSplashDroplets), 3.0, Puddle) + 0.5))
    {
        return CulledRainVertex();
    }

    const float3 Launch   = Normal * Up + (Tangent * cos(Angle) + Bitangent * sin(Angle)) * Spread;
    const float3 Head     = Splash.Position + Launch * Age + 0.5 * kGravity * Age * Age;
    const float3 Velocity = Launch + kGravity * Age;
    const float3 Tail     = Head - Velocity * 0.02;

    if (DropletOpacity <= 0.0)
    {
        return CulledRainVertex();
    }
    return ExpandStreak(Head, Tail, 0.0012 * Size, Corner, Color, DropletOpacity * (1.0 - T));
}
