#pragma once
#include "Common.slang"

// Display transform: linear HDR radiance -> display-referred [0, 1] -> encoded for the swapchain.
//
// Shared by the scene's post-process pass (ColorGrading.slang) and the editor's HDR texture preview
// (ImGuiPixel.slang). Both write to the same _UNORM swapchain, so both owe the same encode; keeping
// one copy is what makes a texture preview and the same texture in-world read alike.

#define TONEMAPPER_NONE       0u
#define TONEMAPPER_ACES       1u
#define TONEMAPPER_AGX        2u
#define TONEMAPPER_AGX_PUNCHY 3u
#define TONEMAPPER_AGX_GOLDEN 4u

float3 Tonemap_ACES(float3 X)
{
    static const float A = 2.51;
    static const float B = 0.03;
    static const float C = 2.43;
    static const float D = 0.59;
    static const float E = 0.14;
    return saturate((X * (A * X + B)) / (X * (C * X + D) + E));
}

// AGX -- bwrensch's polynomial fit of Troy Sobotka's AGX (linear -> display-referred [0, 1]).
// https://github.com/sobotka/AgX  https://iolite-engine.com/blog_posts/minimal_agx_implementation
float3 AGX_Polynomial(float3 X)
{
    float3 X2 = X  * X;
    float3 X4 = X2 * X2;
    return + 15.5     * X4 * X2
           - 40.14    * X4 * X
           + 31.96    * X4
           - 6.868    * X2 * X
           + 0.4298   * X2
           + 0.1191   * X
           - 0.00232;
}

float3 Tonemap_AGX_Base(float3 X)
{
    static const float3x3 AGX_IN = float3x3(
        0.842479062253094, 0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772, 0.0791661274605434,
        0.0423756549057051, 0.0784336000000000, 0.879142973793104);

    static const float MIN_EV = -12.47393;
    static const float MAX_EV =  4.026069;

    X = max(X, 0.0);
    X = mul(AGX_IN, X);
    X = clamp(log2(X), MIN_EV, MAX_EV);
    X = (X - MIN_EV) / (MAX_EV - MIN_EV);
    return AGX_Polynomial(X);
}

// "Look" stage applied in the AGX log space after the polynomial sigmoid
// but before EOTF. Slope, power and offset are per-channel CDL-style.
float3 AGX_Look(float3 X, float3 Slope, float3 Power, float Saturation)
{
    float Luma = Luminance(X);
    X = pow(X * Slope, Power);
    return Luma + Saturation * (X - Luma);
}

float3 Tonemap_AGX(float3 X)
{
    return Tonemap_AGX_Base(X);
}

float3 Tonemap_AGX_Punchy(float3 X)
{
    X = Tonemap_AGX_Base(X);
    return AGX_Look(X, float3(1.0), float3(1.35), 1.4);
}

float3 Tonemap_AGX_Golden(float3 X)
{
    X = Tonemap_AGX_Base(X);
    return AGX_Look(X, float3(1.0, 0.9, 0.5), float3(0.8), 0.8);
}

float3 ApplyTonemap(float3 X, uint Mode)
{
    switch (Mode)
    {
        default:
        case TONEMAPPER_NONE:       return saturate(X);
        case TONEMAPPER_ACES:       return Tonemap_ACES(X);
        case TONEMAPPER_AGX:        return Tonemap_AGX(X);
        case TONEMAPPER_AGX_PUNCHY: return Tonemap_AGX_Punchy(X);
        case TONEMAPPER_AGX_GOLDEN: return Tonemap_AGX_Golden(X);
    }
}

// OETF for the swapchain. Every color target the compositor presents is _UNORM, never _UNORM_SRGB,
// so nothing encodes for us -- a pass that skips this ships raw linear values to the display and
// reads far too dark in the shadows, which is exactly where HDR content lives.
float3 EncodeDisplayGamma(float3 X)
{
    static const float INV_GAMMA = 1.0 / 2.2;
    return pow(max(X, 0.0), float3(INV_GAMMA));
}
