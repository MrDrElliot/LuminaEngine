#include "Includes/RainCommon.slang"

// Light a drop sends back from its middle, with the rim's Fresnel reflection making up the rest.
static const float kCoreLight   = 0.7;
static const float kRimGain     = 4.0;
// How wide a cone behind the drop its lens squeezes into its own width, in drop widths.
static const float kLensSpread  = 3.0;

float StreakShape(float Across, float Along, bool bLens)
{
    // A lensing drop is a solid cylinder edge to edge, a plain streak a soft smear.
    const float Falloff = bLens ? 1.5 : 3.0;
    return exp(-Across * Across * Falloff) * smoothstep(0.0, 0.12, Along) * (1.0 - 0.6 * smoothstep(0.6, 1.0, Along));
}

float3 StreakRadiance(FRainVertexOutput In)
{
    const float Across  = In.Coord.x;
    const float Facing  = sqrt(saturate(1.0 - Across * Across));
    const float Fresnel = 0.02 + 0.98 * pow(1.0 - Facing, 5.0);
    float3 Radiance = In.Color * lerp(1.0, kCoreLight + Fresnel * kRimGain, saturate(In.Lens.w));

    const FTexture2D SceneColor = RainArgs().SceneColor;
    if (In.Lens.z > 0.0 && SceneColor.IsValid())
    {
        // A ball lens images the scene behind it upside down, so each side of the drop shows what lies past the other.
        const float2 Offset    = -Across * In.Lens.xy * (kLensSpread * In.Lens.z);
        const float2 UV        = (In.Position.xy + Offset) / float2(GetScreenSize());
        const float3 Refracted = SceneColor.SampleLevel(Samplers::LinearClamp, UV, 0.0).rgb;
        Radiance += Refracted * In.Transmit * (1.0 - Fresnel);
    }
    return Radiance;
}

[shader("pixel")]
float4 PixelMain(FRainVertexOutput In) : SV_Target0
{
    float  Shape;
    float3 Radiance = In.Color;
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
    else if (In.Kind == kRainKindMist)
    {
        // Spray thins toward its edge and never shows the quad it is drawn on.
        const float2 Centered = In.Coord * 2.0 - 1.0;
        const float  R2       = dot(Centered, Centered);
        Shape = exp(-R2 * 3.0) * saturate(1.0 - R2);
    }
    else
    {
        Shape    = StreakShape(In.Coord.x, In.Coord.y, In.Lens.z > 0.0);
        Radiance = StreakRadiance(In);
    }

    float Soft = 1.0;
    const FTexture2D SceneDepth = Scene().SceneDepth;
    if (SceneDepth.IsValid() && RainArgs().SoftFadeDistance > 0.0)
    {
        const float SceneRaw = SceneDepth.Load(int3(int2(In.Position.xy), 0)).r;
        Soft = saturate((LinearizeSceneDepth(SceneRaw) - In.ViewDepth) / RainArgs().SoftFadeDistance);
    }

    const float Alpha = saturate(In.Alpha * Shape * Soft);
    return float4(Radiance * Alpha, Alpha);
}
