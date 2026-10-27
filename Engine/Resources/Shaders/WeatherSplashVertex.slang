#include "Includes/WeatherInteraction.slang"
#include "Includes/RainCommon.slang"

static const float3 kGravity            = float3(0.0, -9.8, 0.0);
static const uint   kCrownSegments      = 10u;
static const uint   kMaxDroplets        = 24u;
static const uint   kFirstDropletElement = 1u + kCrownSegments;

// A footfall or a body hitting water, drawn as a spreading ring, a rising crown sheet and droplets thrown off its rim.
[shader("vertex")]
FRainVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    if (!RainArgs().Splashes.Contains(InstanceIndex))
    {
        return CulledRainVertex();
    }

    const FRainSplash Splash = RainArgs().Splashes.Get(InstanceIndex);
    const float Scale    = f16tof32(Splash.PackedLightB >> 16u);
    const float Lifetime = 0.45 + 0.45 * saturate(Scale);
    const float Age      = GetTime() - Splash.SpawnTime;
    if (Splash.Seed == 0u || Age < 0.0 || Age > Lifetime)
    {
        return CulledRainVertex();
    }

    const float  T       = Age / Lifetime;
    const float3 Normal  = UnpackUnitVector(Splash.PackedNormal);
    const float2 LightRG = UnpackHalf2(Splash.PackedLightRG);
    const float3 Color   = float3(LightRG, f16tof32(Splash.PackedLightB & 0xFFFFu));
    // Faded right at the lens, where a splash would fill the screen.
    const float  Distance = length(Splash.Position - GetCameraPosition());
    const float  Opacity  = saturate((Distance - 0.3) / 1.0);

    const uint   Element = VertexIndex / 6u;
    const float2 Corner  = kRainQuadCorners[VertexIndex % 6u];
    const float3 Tangent   = normalize(abs(Normal.y) < 0.99 ? cross(Normal, float3(0.0, 1.0, 0.0)) : float3(1.0, 0.0, 0.0));
    const float3 Bitangent = cross(Normal, Tangent);

    if (Element == 0u)
    {
        const float  Radius = (0.06 + 0.25 * Scale) * (0.2 + 0.8 * sqrt(T));
        const float2 Quad   = float2(Corner.x, Corner.y * 2.0 - 1.0);
        const float3 World  = Splash.Position + Normal * 0.006 + (Tangent * Quad.x + Bitangent * Quad.y) * Radius;

        FRainVertexOutput Out;
        Out.Position  = WorldToClip(World);
        Out.Color     = Color;
        Out.Coord     = Quad * 0.5 + 0.5;
        Out.Alpha     = Opacity * 0.55 * (1.0 - T) * (1.0 - T);
        Out.ViewDepth = Out.Position.w;
        Out.Kind      = kRainKindRing;
        return Out;
    }

    if (Element < kFirstDropletElement)
    {
        // Only a real plunge raises a crown, a footstep in a puddle just rings and flicks droplets.
        const float CrownOpacity = saturate((Scale - 0.3) * 2.0);
        if (CrownOpacity <= 0.0)
        {
            return CulledRainVertex();
        }

        const float Segment = float(Element - 1u) + (Corner.x * 0.5 + 0.5);
        const float Angle   = Segment / float(kCrownSegments) * 6.2831853;
        const float Rise    = sin(3.14159 * saturate(T * 1.6));
        const float Base    = 0.04 + 0.16 * Scale * sqrt(T);
        const float Radius  = lerp(Base, Base * 1.3, Corner.y);
        const float Height  = 0.18 * Scale * Rise * Corner.y;
        const float3 World  = Splash.Position + Normal * Height + (Tangent * cos(Angle) + Bitangent * sin(Angle)) * Radius;

        FRainVertexOutput Out;
        Out.Position  = WorldToClip(World);
        Out.Color     = Color;
        Out.Coord     = float2(Corner.x * 0.5 + 0.5, Corner.y);
        Out.Alpha     = Opacity * CrownOpacity * 0.5 * pow(1.0 - T, 1.5);
        Out.ViewDepth = Out.Position.w;
        Out.Kind      = kRainKindCrown;
        return Out;
    }

    const uint Droplet = Element - kFirstDropletElement;
    if (Droplet >= 6u + uint(float(kMaxDroplets - 6u) * saturate(Scale)))
    {
        return CulledRainVertex();
    }

    uint Seed = Splash.Seed ^ (Droplet * 0x9E3779B9u);
    const float Angle  = WeatherRandom(Seed) * 6.2831853;
    const float Up     = (0.7 + 0.6 * WeatherRandom(Seed)) * (1.0 + 1.1 * Scale);
    const float Spread = (0.3 + 0.7 * WeatherRandom(Seed)) * (0.4 + 0.6 * Scale);

    const float3 Launch   = Normal * Up + (Tangent * cos(Angle) + Bitangent * sin(Angle)) * Spread;
    const float3 Offset   = Launch * Age + 0.5 * kGravity * Age * Age;
    // A droplet that has fallen back through the surface is gone.
    if (dot(Offset, Normal) < 0.0 && Age > 0.02)
    {
        return CulledRainVertex();
    }

    const float3 Head     = Splash.Position + Offset;
    const float3 Velocity = Launch + kGravity * Age;
    const float3 Tail     = Head - Velocity * 0.025;
    // Droplets catch the light as they fly, so they read brighter than the sheet they broke from.
    return ExpandStreak(Head, Tail, 0.0025 * (0.6 + Scale), Corner, Color * 1.5, Opacity * 0.8 * (1.0 - T));
}
