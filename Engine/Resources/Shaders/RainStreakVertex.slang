#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

// Fraction of a pixel a drop covers over the exposure, so a fast streak spreads its water thinner.
static const float kStreakEnergyScale = 60.0;

[shader("vertex")]
FRainVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    if (!HasWeather() || !RainArgs().Visible.Contains(InstanceIndex))
    {
        return CulledRainVertex();
    }

    const uint4        Entry = RainArgs().Visible.Get(InstanceIndex);
    const FWeatherData W     = GetWeather();
    const FRainDrop    Drop  = EvaluateRainDrop(W, Entry.x, GetCameraPosition());

    const float  Exposure   = RainArgs().Exposure;
    const float3 Tail       = Drop.Position - Drop.Velocity * Exposure;
    const float  WidthWorld = W.RainDrops.z * 0.001 * Drop.SizeScale;
    const float  StreakLen  = max(length(Drop.Velocity) * Exposure, 1e-3);

    const float2 LightRG = UnpackHalf2(Entry.y);
    const float2 LightBF = UnpackHalf2(Entry.z);
    const float3 Water   = RainWaterTint(W, Entry.x);
    const float3 Color   = float3(LightRG, LightBF.x) * Water * W.RainLook.y + W.RainEmissive.rgb;
    const float  Alpha   = W.RainLook.z * LightBF.y * saturate(WidthWorld * kStreakEnergyScale / StreakLen);

    FRainVertexOutput Out = ExpandStreak(Drop.Position, Tail, WidthWorld, kRainQuadCorners[VertexIndex % 6u], Color, Alpha);
    Out.Lens.z   = W.RainEmissive.w;
    Out.Lens.w   = W.RainOptics.x;
    Out.Transmit = Water;
    return Out;
}
