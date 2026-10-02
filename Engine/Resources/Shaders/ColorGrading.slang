#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/Tonemap.slang"

// Fused post-process pass: linear HDR -> color grading -> tone map -> vignette -> gamma encode.
// Inputs are bindless (HDR/bloom/adapted-luminance by index); the 160 B grading constants block (over
// the 128 B push limit) rides the transient ring, reached through a device-address pointer.
//
// The tone-mapping operators and the display encode live in Includes/Tonemap.slang; the editor's HDR
// texture preview runs the same ones so a texture reads the same in the browser as it does in-world.

struct FColorGradingConstants
{
    // 16 B
    float Exposure;        // linear multiplier built from EV stops on CPU side
    float Contrast;        // 1.0 == identity, around mid-grey 0.18
    float Saturation;      // 1.0 == identity (Rec.709 luma)
    float Gamma;           // 1.0 == identity (display-space)

    // 16 B
    float WhiteTemp;       // -1..1, cool->warm
    float WhiteTint;       // -1..1, green->magenta
    float VignetteIntensity;
    float VignetteSmoothness;

    // 16 B
    float VignetteRoundness;
    uint  TonemapMode;
    float Time;
    float BloomIntensity;  // 0 == bloom disabled (bloom not sampled).

    // 80 B
    float4 ColorFilter;    // .rgb tint, .a intensity (0..2)
    float4 Shadows;        // .rgb lift color, .a film grain intensity
    float4 Midtones;       // .rgb gamma color, .a film grain cell size (px)
    float4 Highlights;     // .rgb gain color, .a film grain shadow response
    float4 VignetteColor;  // .rgb tint, .a unused

    // 16 B
    float4 BloomTint;          // .rgb tint, .a chromatic aberration intensity

    // 16 B
    float AutoExposureKey;     // middle-grey key luminance; <= 0 disables auto-exposure
    float AutoExposureMinMul;  // clamp on the auto-exposure multiplier (2^MinEV)
    float AutoExposureMaxMul;  // clamp on the auto-exposure multiplier (2^MaxEV)
    uint  Features;            // GRADING_* bits, set CPU side for every stage not at identity

    float4 Fade;               // .rgb display color, .a amount
    float4 Letterbox;          // .x picture aspect or 0, .y output aspect
};

// Per-pass block: grading constants by device address + bindless indices for the three inputs.
struct FComposePushConstants
{
    uint64_t Constants;   // FColorGradingConstants, read field by field in FragmentMain
    uint HDRIndex;          // bindless SRV: pre-tone-map HDR
    uint BloomIndex;        // bindless SRV: bloom chain mip 0
    uint AdaptedLumIndex;   // bindless SRV: 1x1 adapted luminance
    uint _Pad;
};
FComposePushConstants* Pass() { return GetPassArgs<FComposePushConstants>(); }

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// White balance via LMS chromatic adaptation; maps [-1, 1] knobs onto a CIECAM02-style scale.
float3 WhiteBalance(float3 X, float Temperature, float Tint)
{
    float t1 = Temperature * 0.05;
    float t2 = Tint        * 0.05;

    // Reference white balance points, pre-baked in LMS.
    float x = 0.31271 - t1 * (t1 < 0 ? 0.1 : 0.05);
    float y = 0.32902 + (x - 0.31271) * (-3.0) + t2 * 0.05;

    float3 w1 = float3(0.949237, 1.03542, 1.08728);
    float Y  = 1.0;
    float X_ = Y * x / y;
    float Z  = Y * (1.0 - x - y) / y;

    float L =  0.7328 * X_ + 0.4296 * Y - 0.1624 * Z;
    float M = -0.7036 * X_ + 1.6975 * Y + 0.0061 * Z;
    float S =  0.0030 * X_ + 0.0136 * Y + 0.9834 * Z;
    float3 w2 = float3(L, M, S);

    float3 Balance = w1 / w2;

    static const float3x3 LIN_2_LMS = float3x3(
        3.90405e-1, 5.49941e-1, 8.92632e-3,
        7.08416e-2, 9.63172e-1, 1.35775e-3,
        2.31082e-2, 1.28021e-1, 9.36245e-1);

    static const float3x3 LMS_2_LIN = float3x3(
         2.85847,  -1.62879, -2.48910e-2,
        -2.10182e-1, 1.15820,  3.24281e-4,
        -4.18120e-2,-1.18169e-1, 1.06867);

    float3 lms = mul(LIN_2_LMS, X);
    lms       *= Balance;
    return mul(LMS_2_LIN, lms);
}

// Contrast pivots on 18% grey; saturation pivots on Rec.709 luma so neutrals don't drift.
float3 ApplyContrast(float3 X, float Contrast)
{
    static const float3 MidGrey = float3(0.18);
    return (X - MidGrey) * Contrast + MidGrey;
}

float3 ApplySaturation(float3 X, float Saturation)
{
    float Luma = Luminance(X);
    return lerp(float3(Luma), X, Saturation);
}

// Lift/Gamma/Gain (CDL-ish): Lift pulls shadows (weighted 1-X), Gamma the midtones, Gain the
// highlights (weighted X). Each color knob is centered on 1.0 (identity).
float3 LiftGammaGain(float3 X, float3 Shadows, float3 Midtones, float3 Highlights)
{
    float3 Lift = Shadows  - 1.0;     // signed
    float3 Gain = Highlights;         // identity at 1.0
    float3 Mid  = max(Midtones, 0.001);

    X = X * Gain + Lift * (1.0 - X);
    X = max(X, 0.0);
    X = pow(X, 1.0 / Mid);            // higher Midtone color -> brighter mids
    return X;
}

// Vignette (display space). Roundness 0 = viewport aspect, 1 = circle; Smoothness = falloff width.
float3 ApplyVignette(float3 X, float2 UV, float Intensity, float Roundness, float Smoothness, float3 Tint)
{
    if (Intensity <= 0.0) return X;

    float2 Center = UV - 0.5;
    // Aspect-correct unless Roundness==1.
    Center.x *= lerp(2.0, 1.0, Roundness);
    float Dist = length(Center);

    // Inner radius shrinks with intensity so the falloff stays controllable.
    float Inner = lerp(0.7, 0.0, Intensity);
    float Outer = Inner + max(Smoothness, 0.001);
    float V     = 1.0 - smoothstep(Inner, Outer, Dist);

    return lerp(Tint, X, V);
}

// Film grain: time-decorrelated per-pixel hash. Size quantizes coords into cells (coarser stock),
// Response biases toward shadows. Applied multiplicatively in display space, pre-sRGB.
float FilmGrainHash(float2 P, float Time)
{
    P += Time * float2(47.0, 17.0);
    float3 P3 = frac(float3(P.xyx) * float3(0.1031, 0.1030, 0.0973));
    P3 += dot(P3, P3.yzx + 33.33);
    return frac((P3.x + P3.y) * P3.z);
}

float3 ApplyFilmGrain(float3 X, float2 PixelPos, float Time, float Intensity, float Size, float Response)
{
    if (Intensity <= 0.0) return X;

    float2 Cell  = floor(PixelPos / max(Size, 1.0));
    float  Noise = FilmGrainHash(Cell, Time) - 0.5;        // signed [-0.5, 0.5]

    // Brighter pixels get less grain when Response > 0 (film-stock shoulder behavior).
    float Luma   = Luminance(X);
    float Falloff = lerp(1.0, 1.0 - smoothstep(0.0, 0.85, Luma), saturate(Response));

    return X * (1.0 + Noise * Intensity * Falloff);
}

// Mirrors EGradingFeature in SceneRenderer_Post.cpp.
static const uint GRADING_CHROMATIC_ABERRATION = 1u << 0;
static const uint GRADING_BLOOM                = 1u << 1;
static const uint GRADING_AUTO_EXPOSURE        = 1u << 2;
static const uint GRADING_WHITE_BALANCE        = 1u << 3;
static const uint GRADING_COLOR_FILTER         = 1u << 4;
static const uint GRADING_LIFT_GAMMA_GAIN      = 1u << 5;
static const uint GRADING_CONTRAST             = 1u << 6;
static const uint GRADING_SATURATION           = 1u << 7;
static const uint GRADING_VIGNETTE             = 1u << 8;
static const uint GRADING_GAMMA                = 1u << 9;
static const uint GRADING_FILM_GRAIN           = 1u << 10;
static const uint GRADING_FADE                 = 1u << 11;
static const uint GRADING_LETTERBOX            = 1u << 12;

[shader("fragment")]
float4 FragmentMain(VSOutput Input) : SV_Target0
{
    // Fields are read through the pointer inside each stage, so a disabled stage costs neither its loads nor its math.
    FColorGradingConstants* Grading = (FColorGradingConstants*)Pass().Constants;
    const uint Features = Grading.Features;

    SamplerState Smp    = gSamplers[SAMPLER_LINEAR_CLAMP];
    Texture2D    HDRTex = gTextures2D[Pass().HDRIndex];

    float3 Color;
    if ((Features & GRADING_CHROMATIC_ABERRATION) != 0u)
    {
        float2 Center = Input.UV - 0.5;
        float2 Offset = Center * Grading.BloomTint.a;
        Color.r = HDRTex.Sample(Smp, Input.UV + Offset).r;
        Color.g = HDRTex.Sample(Smp, Input.UV).g;
        Color.b = HDRTex.Sample(Smp, Input.UV - Offset).b;
    }
    else
    {
        Color = HDRTex.Sample(Smp, Input.UV).rgb;
    }

    if ((Features & GRADING_BLOOM) != 0u)
    {
        float3 Bloom = gTextures2D[Pass().BloomIndex].SampleLevel(Smp, Input.UV, 0.0).rgb;
        Color += Bloom * Grading.BloomTint.rgb * Grading.BloomIntensity;
    }

    float AEScale = 1.0;
    if ((Features & GRADING_AUTO_EXPOSURE) != 0u)
    {
        float AdaptedLum = gTextures2D[Pass().AdaptedLumIndex].SampleLevel(Smp, float2(0.5, 0.5), 0.0).r;
        AEScale = clamp(Grading.AutoExposureKey / max(AdaptedLum, 1e-4),
                        Grading.AutoExposureMinMul, Grading.AutoExposureMaxMul);
    }
    Color *= Grading.Exposure * AEScale;

    if ((Features & GRADING_WHITE_BALANCE) != 0u)
    {
        Color = WhiteBalance(Color, Grading.WhiteTemp, Grading.WhiteTint);
    }

    if ((Features & GRADING_COLOR_FILTER) != 0u)
    {
        const float4 Filter   = Grading.ColorFilter;
        const float3 Filtered = Color * Filter.rgb;
        Color = lerp(Color, Filtered, saturate(Filter.a));
        if (Filter.a > 1.0)
        {
            Color = lerp(Color, Filtered, Filter.a - 1.0);
        }
    }

    Color = ApplyTonemap(Color, Grading.TonemapMode);

    if ((Features & GRADING_LIFT_GAMMA_GAIN) != 0u)
    {
        Color = LiftGammaGain(Color, Grading.Shadows.rgb, Grading.Midtones.rgb, Grading.Highlights.rgb);
    }
    if ((Features & GRADING_CONTRAST) != 0u)
    {
        Color = ApplyContrast(Color, Grading.Contrast);
    }
    Color = max(Color, 0.0);
    if ((Features & GRADING_SATURATION) != 0u)
    {
        Color = max(ApplySaturation(Color, Grading.Saturation), 0.0);
    }

    if ((Features & GRADING_VIGNETTE) != 0u)
    {
        Color = ApplyVignette(Color, Input.UV, Grading.VignetteIntensity, Grading.VignetteRoundness, Grading.VignetteSmoothness, Grading.VignetteColor.rgb);
    }

    if ((Features & GRADING_GAMMA) != 0u)
    {
        Color = pow(max(Color, 0.0), float3(1.0 / Grading.Gamma));
    }

    if ((Features & GRADING_FILM_GRAIN) != 0u)
    {
        Color = max(ApplyFilmGrain(Color, Input.Position.xy, Grading.Time,
                                   Grading.Shadows.a, Grading.Midtones.a, Grading.Highlights.a), 0.0);
    }

    Color = EncodeDisplayGamma(Color);

    Color += ScreenSpaceDither(Input.Position.xy);

    if ((Features & GRADING_FADE) != 0u)
    {
        const float4 Fade = Grading.Fade;
        Color = lerp(Color, Fade.rgb, saturate(Fade.a));
    }

    if ((Features & GRADING_LETTERBOX) != 0u)
    {
        const float2 Letterbox = Grading.Letterbox.xy;
        const float Ratio = Letterbox.y / Letterbox.x;
        const float2 FromCenter = abs(Input.UV - 0.5) * 2.0;
        if ((Ratio < 1.0 && FromCenter.y > Ratio) || (Ratio > 1.0 && FromCenter.x > 1.0 / Ratio))
        {
            Color = float3(0.0, 0.0, 0.0);
        }
    }

    return float4(Color, 1.0);
}
