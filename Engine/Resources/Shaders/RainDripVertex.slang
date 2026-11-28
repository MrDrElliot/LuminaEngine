#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

static const float kDripGravity    = 9.8;
static const float kDripWidthWorld = 0.0035;
static const float kDripRingLife   = 0.3;
// Matches the energy spread of a falling drop's streak in RainStreakVertex.slang.
static const float kDripEnergyScale = 60.0;

// Drops released one after another from a drip site, each falling under gravity and ringing where it lands.
[shader("vertex")]
FRainVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    if (!HasWeather() || !RainArgs().Drips.Contains(InstanceIndex))
    {
        return CulledRainVertex();
    }

    const FRainDrip    Drip = RainArgs().Drips.Get(InstanceIndex);
    const FWeatherData W    = GetWeather();
    const float Age      = W.Surface2.z - Drip.SpawnTime;
    const float FallTime = sqrt(2.0 * Drip.FallHeight / kDripGravity);
    if (Drip.Seed == 0u || Age < 0.0 || Age > kDripSiteLifetime + FallTime + kDripRingLife)
    {
        return CulledRainVertex();
    }

    uint Seed = Drip.Seed;
    const float Period = 0.3 + 0.6 * WeatherRandom(Seed);

    const uint  Element = VertexIndex / 6u;
    const uint  Drop    = Element >> 1u;
    const bool  bRing   = (Element & 1u) != 0u;
    const float Release = (floor(Age / Period) - float(Drop)) * Period;
    const float Fallen  = Age - Release;
    if (Release < 0.0 || Release > kDripSiteLifetime)
    {
        return CulledRainVertex();
    }

    const float3 Water    = RainWaterTint(W, Drip.Seed);
    const float3 Color    = float3(UnpackHalf2(Drip.PackedLightRG), UnpackHalf(Drip.PackedLightB)) * Water * W.RainLook.y + W.RainEmissive.rgb;
    const float  Distance = length(Drip.Origin - GetCameraPosition());
    const float  Opacity  = W.RainLook.z * saturate((Distance - 0.5) / 1.0) * saturate((W.RainTint.w - Distance) / (W.RainTint.w * 0.25));
    const float2 Corner   = kRainQuadCorners[VertexIndex % 6u];

    if (bRing)
    {
        const float RingT = (Fallen - FallTime) / kDripRingLife;
        if (RingT < 0.0 || RingT > 1.0)
        {
            return CulledRainVertex();
        }
        const float  Radius  = 0.02 + 0.09 * sqrt(RingT);
        const float3 Landing = Drip.Origin - float3(0.0, Drip.FallHeight - 0.004, 0.0);
        return ExpandQuad(Landing, float3(Radius, 0.0, 0.0), float3(0.0, 0.0, Radius), Corner, Color,
                          Opacity * 0.5 * (1.0 - RingT) * (1.0 - RingT), kRainKindRing);
    }

    if (Fallen >= FallTime)
    {
        return CulledRainVertex();
    }

    const float  Speed     = kDripGravity * Fallen;
    const float3 Head      = Drip.Origin - float3(0.0, 0.5 * kDripGravity * Fallen * Fallen, 0.0);
    const float  StreakLen = max(Speed * RainArgs().Exposure, 0.004);
    const float3 Tail      = Head + float3(0.0, StreakLen, 0.0);
    const float  Alpha     = Opacity * saturate(kDripWidthWorld * kDripEnergyScale / StreakLen);

    FRainVertexOutput Out = ExpandStreak(Head, Tail, kDripWidthWorld, Corner, Color, Alpha);
    Out.Lens.z   = W.RainEmissive.w;
    Out.Lens.w   = W.RainOptics.x;
    Out.Transmit = Water;
    return Out;
}
