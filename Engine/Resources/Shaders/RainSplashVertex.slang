#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

static const float3 kGravity = float3(0.0, -9.8, 0.0);

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
    if (Splash.Seed == 0u || Age < 0.0 || Age > kSplashLifetime)
    {
        return CulledRainVertex();
    }

    const float  T       = Age / kSplashLifetime;
    const float3 Normal  = UnpackNormal(Splash.PackedNormal);
    const float2 LightRG = UnpackHalf2(Splash.PackedLightRG);
    const float3 Color   = float3(LightRG, f16tof32(Splash.PackedLightB)) * W.RainTint.rgb * W.RainLook.y;
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

    if (Element == 0u)
    {
        const float  Radius = Size * (0.01 + 0.05 * sqrt(T));
        const float2 Quad   = float2(Corner.x, Corner.y * 2.0 - 1.0);
        const float3 World  = Splash.Position + Normal * 0.004 + (Tangent * Quad.x + Bitangent * Quad.y) * Radius;

        FRainVertexOutput Out;
        Out.Position  = WorldToClip(World);
        Out.Color     = Color;
        Out.Coord     = Quad * 0.5 + 0.5;
        Out.Alpha     = Opacity * 0.35 * (1.0 - T) * (1.0 - T);
        Out.ViewDepth = Out.Position.w;
        Out.Kind      = kRainKindRing;
        return Out;
    }

    for (uint Skip = 1u; Skip < Element; ++Skip)
    {
        WeatherRandom(Seed);
        WeatherRandom(Seed);
        WeatherRandom(Seed);
    }
    const float Angle  = WeatherRandom(Seed) * TWO_PI;
    const float Up     = (1.1 + 1.1 * WeatherRandom(Seed)) * sqrt(Size);
    const float Spread = (0.4 + 0.8 * WeatherRandom(Seed)) * sqrt(Size);

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
