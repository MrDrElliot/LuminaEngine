#pragma once
#include "SceneGlobals.slang"

// Hashed alpha testing (Wyman and McGuire, I3D 2017), anchored in world space so it is stable under camera motion.

// Grain of the noise. Larger = finer cells.
static const float kHashScale = 1.0;

// Meters. Near geometry reads fine with the hard cutoff, so the noise fades in with distance.
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
    // Ahead of the early-out, because a quad straddling the fade start would otherwise differentiate with lanes gone.
    const float MaxDeriv = max(length(ddx(Anchor)), length(ddy(Anchor)));

    const float Fade = saturate((ViewDistance - kHashFadeStart) / max(kHashFadeEnd - kHashFadeStart, 1e-4));
    if (Fade <= 0.0)
    {
        return Cutoff;
    }

    // Cell sized to the pixel footprint, so the grain stays constant on screen as the surface recedes.
    const float PixScale = 1.0 / max(kHashScale * MaxDeriv, 1e-8);

    // Two power-of-two cell sizes, blended, so the grain does not visibly step as the surface moves.
    const float2 PixScales = float2(exp2(floor(log2(PixScale))), exp2(ceil(log2(PixScale))));
    const float2 Alpha     = float2(HashedAlpha_Hash3D(floor(PixScales.x * Anchor)),
                                    HashedAlpha_Hash3D(floor(PixScales.y * Anchor)));

    const float LerpFactor = frac(log2(PixScale));
    const float X          = lerp(Alpha.x, Alpha.y, LerpFactor);

    // The paper's inverse CDF, returning the triangular blend of two uniforms to uniform so coverage matches alpha.
    const float  A     = max(min(LerpFactor, 1.0 - LerpFactor), 1e-6);
    const float3 Cases = float3(X * X / (2.0 * A * (1.0 - A)),
                                (X - 0.5 * A) / (1.0 - A),
                                1.0 - ((1.0 - X) * (1.0 - X) / (2.0 * A * (1.0 - A))));
    const float Threshold = (X < (1.0 - A)) ? ((X < A) ? Cases.x : Cases.y) : Cases.z;

    return lerp(Cutoff, clamp(Threshold, 1.0e-6, 1.0), Fade);
}
