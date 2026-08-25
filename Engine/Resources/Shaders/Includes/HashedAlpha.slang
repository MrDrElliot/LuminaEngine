#pragma once
#include "SceneGlobals.slang"

// Hashed alpha testing (Wyman and McGuire, I3D 2017). A hard cutoff makes a leaf's coverage a step
// function, so sub-pixel foliage pops in and out and its edges crawl. A per-pixel stochastic cutoff turns
// that step into dithered coverage that is stable on the surface instead of on the screen.
// The anchor is world position, which is stable under camera motion. It drifts under wind displacement,
// but the distance fade has already handed those pixels back to the deterministic test by then.

// Grain of the noise. Larger = finer cells.
static const float kHashScale = 1.0;

// Fading in noise with distance (paper section 4). Near geometry has pixels to spare and the hard cutoff
// reads fine, while the stochastic pattern would be the most visible thing on screen. Metres.
static const float kHashFadeStart = 8.0;
static const float kHashFadeEnd   = 25.0;

float HashedAlpha_Hash1D(float2 V)
{
    return frac(1.0e4 * sin(17.0 * V.x + 0.1 * V.y) * (0.1 + abs(sin(13.0 * V.y + V.x))));
}

float HashedAlpha_Hash3D(float3 V)
{
    // Phase re-rolls the cell per frame, so a two-frame resolve averages two coverage samples per leaf.
    const float Phase = float(Scene().TemporalPhase) * 0.7548776662;
    return HashedAlpha_Hash1D(float2(HashedAlpha_Hash1D(V.xy + Phase), V.z + Phase));
}

// The cutoff this pixel compares Opacity against. Pixel lanes only, since it needs a quad.
float HashedAlphaThreshold(float3 Anchor, float ViewDistance, float Cutoff)
{
    const float Fade = saturate((ViewDistance - kHashFadeStart) / max(kHashFadeEnd - kHashFadeStart, 1e-4));
    if (Fade <= 0.0)
    {
        return Cutoff;
    }

    // Cell sized to the pixel footprint, so the grain stays constant on screen as the surface recedes.
    const float MaxDeriv = max(length(ddx(Anchor)), length(ddy(Anchor)));
    const float PixScale = 1.0 / max(kHashScale * MaxDeriv, 1e-8);

    // Two power-of-two cell sizes, blended, so the grain does not visibly step as the surface moves.
    const float2 PixScales = float2(exp2(floor(log2(PixScale))), exp2(ceil(log2(PixScale))));
    const float2 Alpha     = float2(HashedAlpha_Hash3D(floor(PixScales.x * Anchor)),
                                    HashedAlpha_Hash3D(floor(PixScales.y * Anchor)));

    const float LerpFactor = frac(log2(PixScale));
    const float X          = lerp(Alpha.x, Alpha.y, LerpFactor);

    // Blending two uniform variables gives a triangular distribution, which biases the threshold toward
    // 0.5 and stops coverage matching alpha. This is the paper's inverse CDF putting it back to uniform.
    const float  A     = max(min(LerpFactor, 1.0 - LerpFactor), 1e-6);
    const float3 Cases = float3(X * X / (2.0 * A * (1.0 - A)),
                                (X - 0.5 * A) / (1.0 - A),
                                1.0 - ((1.0 - X) * (1.0 - X) / (2.0 * A * (1.0 - A))));
    const float Threshold = (X < (1.0 - A)) ? ((X < A) ? Cases.x : Cases.y) : Cases.z;

    return lerp(Cutoff, clamp(Threshold, 1.0e-6, 1.0), Fade);
}
