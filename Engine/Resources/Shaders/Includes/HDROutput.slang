#pragma once
#include "Tonemap.slang"

// Mirrored by FHDROutputArgs in RenderManager.cpp.
struct FHDROutputArgs
{
    FTexture2D Composite;
    float      GainStops;
    float      PaperWhiteNits;
    float      PeakNits;
};

// How far the SDR image's highlights are lifted, 1 through the midtones and MaxGain for the hottest pixels.
float HDRHighlightGain(float PeakChannel, float MaxGain)
{
    // The fourth power overflows float far past any lit pixel, so the clamp keeps the ratio finite.
    const float X  = clamp(PeakChannel, 0.0, 1.0e4);
    const float X2 = X * X;
    const float R2 = MaxGain * MaxGain;
    return sqrt(sqrt((1.0 + X2 * X2) / (1.0 + (X2 * X2) / (R2 * R2))));
}

// Gain lives in the display image's alpha as a log fraction, so UI blended over it pulls the gain back toward 1.
float EncodeHDRGain(float Gain, float GainStops)
{
    return GainStops > 0.0 ? saturate(1.0 - log2(max(Gain, 1.0)) / GainStops) : 1.0;
}

float DecodeHDRGain(float Code, float GainStops)
{
    return exp2((1.0 - saturate(Code)) * GainStops);
}

// Display light relative to paper white, from a display-encoded color and the gain its alpha carries.
float3 DecodeDisplayComposite(float4 Encoded, float GainStops)
{
    return DecodeDisplayGamma(Encoded.rgb) * DecodeHDRGain(Encoded.a, GainStops);
}

// ITU-R BT.2087, linear Rec.709 primaries to linear Rec.2020.
float3 Rec709ToRec2020(float3 C)
{
    static const float3x3 M = float3x3(
        0.6274040, 0.3292820, 0.0433136,
        0.0690970, 0.9195400, 0.0113612,
        0.0163916, 0.0880132, 0.8955950);
    return mul(M, C);
}

// SMPTE ST 2084 inverse EOTF over absolute luminance.
float3 EncodePQ(float3 Nits)
{
    static const float M1 = 0.1593017578125;
    static const float M2 = 78.84375;
    static const float C1 = 0.8359375;
    static const float C2 = 18.8515625;
    static const float C3 = 18.6875;

    const float3 Y = pow(saturate(Nits / 10000.0), float3(M1));
    return pow((C1 + C2 * Y) / (1.0 + C3 * Y), float3(M2));
}

// The HDR10 signal for a color in Rec.709 display light, where 1 is paper white.
float3 EncodeHDR10(float3 DisplayLight, float PaperWhiteNits, float PeakNits)
{
    const float3 Nits = Rec709ToRec2020(max(DisplayLight, 0.0)) * PaperWhiteNits;
    return EncodePQ(min(Nits, float3(PeakNits)));
}
