// No rasterizer in a compute entry point, so nothing reached from here may use ddx/ddy.
#define SHADING_COMPUTE 1

#include "Includes/SceneGlobals.slang"
#include "Includes/GroundFog.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/LocalIrradiance.slang"
#include "Includes/GlobalRHI.slang"

// Marches only the stretch of each view ray inside a ground fog layer, at half resolution for AtmosphereComposite.

struct FPushConstants
{
    FTexture2D   Depth;
    FRWTexture2D Output;         // rgb in-scatter, a transmittance
    FRWTexture2D OutputDepth;    // raw depth each texel marched against
    uint         ScreenW;

    uint ScreenH;
    uint TraceW;
    uint TraceH;
    uint Steps;

    GPUSpan<FGroundFogLayer> Layers;

    FTexture2D Flow;
    FTexture2D Fill;
    float2     SimOrigin;

    float SimInvWidth;
    float Time;
    uint  _Pad0;
    uint  _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const float kInvFourPi = 0.07957747155;
// Local lights closer than this would spike a single texel of fog into a hot spot.
static const float kLightMinDistance = 0.4;
// Fog deep in the layer sees less of the sky than fog at its top.
static const float kFloorSkyFraction = 0.55;
// Light reaching into the layer is softened by the light the layer scatters forward on its own.
static const float kSunPathScale = 0.5;
static const float kMultiScatter     = 0.75;
// Lets a low sun still light the layer's top through the bounces, as it would a soft cloud.
static const float kMultiScatterWrap = 0.25;

float HenyeyGreenstein(float CosTheta, float G)
{
    const float G2 = G * G;
    const float Denom = max(1.0 + G2 - 2.0 * G * CosTheta, 1.0e-4);
    return kInvFourPi * (1.0 - G2) / (Denom * sqrt(Denom));
}

// The light reaching a point of the layer, lit once per layer at the point its extinction was centered on.
float3 LightLayer(FGroundFogLayer Layer, FGroundFogColumn Column, float3 P, float3 RayDir)
{
    const float Thickness = Layer.Shape.y;
    const float Height    = P.y - Layer.Shape.x;
    const float Top       = Thickness * clamp(Column.Fill, 0.0, kGroundFogMaxRise);
    const float Depth01   = saturate(Height / max(Top, 1.0e-3));

    float3 Light = GetVolumetricAmbientRadiance() * lerp(kFloorSkyFraction, 1.0, Depth01);
    Light += ClusteredLocalIrradiance(P, kLightMinDistance) * (0.5 * INV_PI);

    if (GetNumLights() > 0u && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional))
    {
        const FLight Sun   = GetLightAt(0);
        const float3 ToSun = normalize(Sun.Direction);
        if (ToSun.y > -0.1)
        {
            const float Below    = max(Top - Height, 0.0);
            const float Path     = Below / max(ToSun.y, 0.15);
            const float Through  = exp(-Layer.Shape.w * Path * kSunPathScale);
            const float Shadow   = SampleSunShadowPoint(Sun, P);
            // Sun.Direction points toward the sun, and the photon travels the other way.
            const float CosTheta = dot(-ToSun, -RayDir);
            const float Phase    = lerp(kInvFourPi, HenyeyGreenstein(CosTheta, Layer.Edge.w), 0.7);
            // Dense fog is lit mostly by light that bounced around inside it, which spreads like a diffuse surface and survives deeper than the direct beam.
            const float Bounced  = kMultiScatter * saturate(ToSun.y + kMultiScatterWrap) * INV_PI * sqrt(Through);
            Light += UnpackColor(Sun.Color).rgb * Sun.Intensity * Shadow * (Phase * Through + Bounced);
        }
    }
    return Light * Layer.Albedo.rgb * Layer.Albedo.w;
}

float4 MarchLayer(FGroundFogLayer Layer, float3 CamPos, float3 RayDir, float TSurface, float Jitter)
{
    const float  ViewDistance = Layer.Edge.z;
    const float2 Segment = GroundFog_RaySegment(Layer, CamPos, RayDir, min(TSurface, ViewDistance));
    if (Segment.x >= Segment.y)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const uint  Steps   = Pass().Steps;
    const float StepLen = (Segment.y - Segment.x) / float(Steps);
    const float Time    = Pass().Time;

    float  Trans    = 1.0;
    float3 Centroid = float3(0.0, 0.0, 0.0);
    float  Weight   = 0.0;

    for (uint i = 0u; i < Steps; ++i)
    {
        if (Trans < 1.0e-3)
        {
            break;
        }

        const float  T = Segment.x + StepLen * (float(i) + Jitter);
        const float3 P = CamPos + RayDir * T;

        const FGroundFogColumn Column = GroundFog_SampleColumn(Pass().Flow, Pass().Fill, Pass().SimOrigin, Pass().SimInvWidth, P.xz);
        const float Fade       = 1.0 - saturate(T / ViewDistance);
        const float Extinction = GroundFog_Extinction(Layer, Column, P, Time) * Fade * Fade;
        if (Extinction <= 0.0)
        {
            continue;
        }

        const float StepTrans = exp(-Extinction * StepLen);
        const float Taken     = Trans * (1.0 - StepTrans);
        Centroid += P * Taken;
        Weight   += Taken;
        Trans    *= StepTrans;
    }

    if (Weight <= 1.0e-5)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // A thin layer is lit nearly the same along its stretch, so one light evaluation at its center of extinction stands in for every step.
    const float3 Center = Centroid / Weight;
    const FGroundFogColumn Column = GroundFog_SampleColumn(Pass().Flow, Pass().Fill, Pass().SimOrigin, Pass().SimInvWidth, Center.xz);
    const float3 Light = LightLayer(Layer, Column, Center, RayDir);
    return float4(Light * (1.0 - Trans), Trans);
}

float4 MarchGroundFog(float2 UV, float2 TexelPos, float Depth)
{
    const float3 CamPos   = GetCameraPosition();
    const float3 RayDir   = GetCameraWorldRayDir(UV);
    const float  TSurface = Depth > 0.0 ? length(ReconstructViewPos(UV, Depth, GetInverseCameraProjection())) : 3.0e38;
    const float  Jitter   = InterleavedGradientNoise(TexelPos);

    float3 InScatter = float3(0.0, 0.0, 0.0);
    float  Trans     = 1.0;
    const uint Count = min(Pass().Layers.Count, GROUND_FOG_MAX_LAYERS);
    for (uint Index = 0u; Index < Count; ++Index)
    {
        const float4 Layer = MarchLayer(Pass().Layers.Get(Index), CamPos, RayDir, TSurface, Jitter);
        InScatter += Trans * Layer.rgb;
        Trans     *= Layer.a;
    }
    return float4(InScatter, Trans);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().TraceW || DTid.y >= Pass().TraceH)
    {
        return;
    }

    const int2   P  = int2(DTid.xy);
    const float2 UV = (float2(P * 2) + 1.0) / float2(Pass().ScreenW, Pass().ScreenH);

    // The nearest of the four covered pixels, as the fog shafts take, so a near edge never marches past its surface.
    const float Depth = Pass().Depth.SampleLevel(Samplers::MaxReduction, UV, 0.0).r;

    Pass().OutputDepth[P] = float4(Depth, 0.0, 0.0, 0.0);
    Pass().Output[P]      = MarchGroundFog(UV, float2(P) + 0.5, Depth);
}
