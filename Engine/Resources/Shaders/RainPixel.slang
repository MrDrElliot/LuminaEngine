#include "Includes/RainCommon.slang"

[shader("pixel")]
float4 PixelMain(FRainVertexOutput In) : SV_Target0
{
    float Shape;
    if (In.Kind == kRainKindRing)
    {
        const float2 Centered = In.Coord * 2.0 - 1.0;
        const float  Radius   = length(Centered);
        Shape = saturate(1.0 - abs(Radius - 0.8) * 6.0) * step(Radius, 1.0);
    }
    else if (In.Kind == kRainKindFlake)
    {
        const float2 Centered = float2(In.Coord.x, In.Coord.y * 2.0 - 1.0);
        Shape = exp(-dot(Centered, Centered) * 2.5);
    }
    else if (In.Kind == kRainKindCrown)
    {
        // A thin sheet that thickens into a bright rim, where the water gathers before it breaks into droplets.
        const float Along = In.Coord.y;
        Shape = (0.3 + 0.7 * smoothstep(0.7, 0.95, Along)) * (1.0 - smoothstep(0.95, 1.0, Along)) * smoothstep(0.0, 0.1, Along);
    }
    else
    {
        const float Across = In.Coord.x;
        const float Along  = In.Coord.y;
        Shape = exp(-Across * Across * 3.0) * smoothstep(0.0, 0.12, Along) * (1.0 - 0.6 * smoothstep(0.6, 1.0, Along));
    }

    float Soft = 1.0;
    const FTexture2D SceneDepth = Scene().SceneDepth;
    if (SceneDepth.IsValid() && RainArgs().SoftFadeDistance > 0.0)
    {
        const float SceneRaw = SceneDepth.Load(int3(int2(In.Position.xy), 0)).r;
        Soft = saturate((LinearizeSceneDepth(SceneRaw) - In.ViewDepth) / RainArgs().SoftFadeDistance);
    }

    const float Alpha = saturate(In.Alpha * Shape * Soft);
    return float4(In.Color * Alpha, Alpha);
}
