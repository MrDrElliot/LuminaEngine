// A compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"

// Brings the traced reflection up to full resolution and swaps it in for the prefiltered environment in each pixel's specular lobe.

struct FSSRResolveArgs
{
    FGBufferTargets  GBuffer;

    FTexture2D Depth;
    FTexture2D SlotImage;   // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    FTexture2D Pyramid;     // mip 0 is the depth each trace texel stood on
    FTexture2D Surface;     // the normal and roughness that went with it

    FTexture2D   Chain;       // the filtered reflection chain
    FTexture2D   MipLevel;    // which level of it each trace texel asked for
    FRWTexture2D HDR;
    uint         Scale;            // screen pixels per trace texel along each axis

    uint  ScreenW;
    uint  ScreenH;
    uint  TraceW;
    uint  TraceH;

    float Intensity;
    float MaxRoughness;
    uint  _Pad1;
    uint  _Pad2;
};
FSSRResolveArgs* Pass() { return GetPassArgs<FSSRResolveArgs>(); }

// How sharply a trace texel loses weight as its depth, normal or roughness drifts from this pixel's.
static const float kDepthFactor     = 2048.0;
static const float kNormalFactor    = 32.0;
static const float kRoughnessFactor = 16.0;

// Combined bilateral weight below which this pixel's neighbors are judged to be some other surface.
static const float kMinCoverage = 0.1;

// Fraction of MaxRoughness at which reflections start handing back to the environment.
static const float kRoughnessFadeStart = 0.75;

float4 SampleTraceTexel(int2 Texel, float Depth, float3 Normal, float Roughness, out float Weight)
{
    const int2   Clamped       = clamp(Texel, int2(0, 0), int2(Pass().TraceW, Pass().TraceH) - 1);
    const float  SampleDepth   = Pass().Pyramid.Load(int3(Clamped, 0)).r;
    const float4 SampleSurface = Pass().Surface.Load(int3(Clamped, 0));
    const float  MipLevel      = Pass().MipLevel.Load(int3(Clamped, 0)).r;

    const float2 UV    = (float2(Clamped) + 0.5) / float2(Pass().TraceW, Pass().TraceH);
    float4       Color = Pass().Chain.SampleLevel(Samplers::LinearClamp, UV, MipLevel);

    // Undoes the luminance compression the trace applied before filtering.
    Color.rgb /= max(1.0 - dot(Color.rgb, float3(0.2126, 0.7152, 0.0722)), 1e-4);

    const float DepthWeight     = exp(-abs(Depth - SampleDepth) * kDepthFactor);
    const float NormalWeight    = exp(-saturate(1.0 - dot(Normal, SampleSurface.xyz)) * kNormalFactor);
    const float RoughnessWeight = exp(-abs(Roughness - SampleSurface.w) * kRoughnessFactor);
    Weight = DepthWeight * NormalWeight * RoughnessWeight;
    return Color;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const int2 Pixel = int2(DTid.xy);
    if (DTid.x >= Pass().ScreenW || DTid.y >= Pass().ScreenH)
    {
        return;
    }

    // Only pixels with a GBuffer, since forward-shaded surfaces leave stale texels behind them.
    if (Pass().SlotImage.Load(Pixel).r < 0.5)
    {
        return;
    }

    const FGBufferTargets Targets = Pass().GBuffer;

    const float Roughness = max(Targets.C.Load(Pixel).r, 0.045);
    const float RoughFade = 1.0 - smoothstep(Pass().MaxRoughness * kRoughnessFadeStart, Pass().MaxRoughness, Roughness);
    if (RoughFade <= 0.0)
    {
        return;
    }

    const float Depth = Pass().Depth.Load(Pixel).r;
    if (Depth <= 0.0)
    {
        return;
    }

    const FGBufferData G = LoadGBuffer(Targets, Pixel);
    if (IsUnlit(G))
    {
        return;
    }

    // The four trace texels around this pixel, each kept only as far as it describes the same surface.
    const float2 TracePos = (float2(Pixel) + 0.5) / float(Pass().Scale) - 0.5;
    const int2   Base     = int2(floor(TracePos));
    const float2 Bilinear = TracePos - float2(Base);

    float  Weights[4];
    float4 Colors[4];
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        Colors[i] = SampleTraceTexel(Base + int2(i & 1, i >> 1), Depth, G.WorldNormal, Roughness, Weights[i]);
    }
    Weights[0] *= (1.0 - Bilinear.x) * (1.0 - Bilinear.y);
    Weights[1] *= Bilinear.x         * (1.0 - Bilinear.y);
    Weights[2] *= (1.0 - Bilinear.x) * Bilinear.y;
    Weights[3] *= Bilinear.x         * Bilinear.y;

    const float  TotalWeight = Weights[0] + Weights[1] + Weights[2] + Weights[3];
    if (TotalWeight <= 0.0)
    {
        return;
    }
    // When none of the four texels describe this surface, the environment is a better answer than whichever is least wrong.
    const float  Coverage   = smoothstep(0.0, kMinCoverage, TotalWeight);
    const float4 Reflection = (Colors[0] * Weights[0] + Colors[1] * Weights[1] + Colors[2] * Weights[2] + Colors[3] * Weights[3]) / TotalWeight * Coverage;
    if (Reflection.a <= 0.0)
    {
        return;
    }

    const float2 UV            = (float2(Pixel) + 0.5) / float2(Pass().ScreenW, Pass().ScreenH);
    const float3 ViewPos       = ReconstructViewPos(UV, Depth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;

    const float3 N     = G.WorldNormal;
    const float3 V     = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);
    const float  NdotV = saturate(dot(N, V));
    const float3 R     = reflect(-V, N);

    // The same terms EvaluateIBLAmbientSplit builds, so only the radiance source is being swapped.
    const float3 F0     = lerp(float3(0.08 * G.Specular), G.BaseColor, G.Metallic);
    const float2 DFG    = SampleBRDFLUT(NdotV, Roughness);
    const float3 FssEss = FresnelSchlickRoughness(NdotV, F0, Roughness) * DFG.x + DFG.y;

    // Same occlusion the base pass gave this lobe, or the reflection's coverage edge reads as a brightness step.
    float AmbientAO = G.AmbientOcclusion;
    if (Scene().GTAOSettings.AOTexture.IsValid())
    {
        AmbientAO *= Scene().GTAOSettings.AOTexture.SampleLevel(Samplers::LinearClamp, UV, 0.0).r;
    }
    const float SpecAO = ComputeSpecularOcclusion(NdotV, AmbientAO, Roughness);

    // Validity is premultiplied into the color, so the environment keeps exactly the share the trace could not vouch for.
    const float3 PreFilt = SampleSpecularRadiance(WorldPosition, R, Roughness);
    const float3 Delta   = FssEss * SpecAO * (Reflection.rgb - PreFilt * Reflection.a) * RoughFade * saturate(Pass().Intensity);

    const float4 Current = Pass().HDR[Pixel];
    Pass().HDR[Pixel] = float4(Current.rgb + Delta, Current.a);
}
