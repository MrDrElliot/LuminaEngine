#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

// A tumbling flake as a soft disc, never thinner than a pixel so a distant flake fades rather than sparkles.
[shader("vertex")]
FRainVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    if (!HasWeather() || !RainArgs().Visible.Contains(InstanceIndex))
    {
        return CulledRainVertex();
    }

    const uint4        Entry = RainArgs().Visible.Get(InstanceIndex);
    const FWeatherData W     = GetWeather();
    const FRainDrop    Flake = EvaluateSnowFlake(W, Entry.x, GetCameraPosition());

    const float  SizeWorld = W.Snow.z * 0.001 * Flake.SizeScale;
    const float3 Tail      = Flake.Position - Flake.Velocity * RainArgs().Exposure;

    const float2 LightRG = UnpackHalf2(Entry.y);
    const float2 LightBF = UnpackHalf2(Entry.z);
    const float3 Color   = float3(LightRG, LightBF.x) * W.SnowLook.x;
    const float  Alpha   = W.SnowLook.y * LightBF.y;

    FRainVertexOutput Out = ExpandStreak(Flake.Position, Tail, SizeWorld, kRainQuadCorners[VertexIndex % 6u], Color, Alpha);
    Out.Kind = kRainKindFlake;
    return Out;
}
