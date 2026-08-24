// Fullscreen postprocess for RmlUi's layer stack, one pass per filter, all colors premultiplied.
#pragma once
#include "Includes/GlobalRHI.slang"

#define UI_FILTER_COPY         0u
#define UI_FILTER_COLOR_MATRIX 1u
#define UI_FILTER_OPACITY      2u
#define UI_FILTER_BLUR         3u
#define UI_FILTER_DROP_SHADOW  4u
#define UI_FILTER_MASK_IMAGE   5u

// Must match RmlUiCommon.slang::FUiClipMask. Duplicated rather than included, because that header
// declares its own Pass() and the two would collide.
struct FUiClipMask
{
    float4 Rect;
    float4 Radii;
    float4 Params;
};

// Matches FUiFilterArgs in RmlUiRenderer.cpp.
struct FUiFilterArgs
{
    float4x4 ColorMatrix;   // @0
    float4   Color;         // @64  drop-shadow tint, premultiplied
    float4   TexelStep;     // @80  .xy = blur step in UV, .zw = drop-shadow UV offset
    float4   SourceRect;    // @96  source UV scale.xy and bias.zw, so a pass can read a sub-rect
    uint     Kind;          // @112
    uint     SourceID;      // @116
    uint     MaskID;        // @120
    uint     SamplerIndex;  // @124
    float    Opacity;       // @128
    uint     TapCount;      // @132 blur taps on each side of center
    float    Sigma;         // @136
    uint     ClipMaskRange; // @140 count in the low 8 bits, first ClipMasks[] index above them
    uint64_t ClipMaskAddress; // @144
    float2   ClipScale;     // @152 document to target pixels
    float2   _Pad0;         // @160
    float2   _Pad1;         // @168

    // Reached, never stored; a typed pointer field lands in an undecorated Function variable.
    property FUiClipMask* ClipMasks { get { return (FUiClipMask*)ClipMaskAddress; } }
};

FUiFilterArgs* Pass() { return GetArgs<FUiFilterArgs>(); }

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

float SdRoundedBox(float2 P, float2 HalfExtent, float4 Radii)
{
    float2 Pair   = (P.x > 0.0) ? Radii.yz : Radii.xw;
    float  Radius = (P.y > 0.0) ? Pair.y : Pair.x;
    float2 Q      = abs(P) - HalfExtent + Radius;
    return min(max(Q.x, Q.y), 0.0) + length(max(Q, 0.0)) - Radius;
}

// RmlUi clips the composite as well as the geometry, which is what rounds a backdrop filter.
float ClipCoverage(FUiFilterArgs Args, float2 FragCoord)
{
    const uint Count  = Args.ClipMaskRange & 0xFFu;
    const uint Offset = Args.ClipMaskRange >> 8u;

    const float2 DocPos = FragCoord / max(Args.ClipScale, float2(1e-5));

    float Coverage = 1.0;
    for (uint i = 0u; i < Count; ++i)
    {
        FUiClipMask Mask = Args.ClipMasks[Offset + i];
        const float Dist  = SdRoundedBox(DocPos - Mask.Rect.xy, Mask.Rect.zw, Mask.Radii);
        const float Width = max(fwidth(Dist), 1e-5);
        const float Cover = saturate(0.5 - Dist / Width);
        Coverage *= (Mask.Params.x != 0.0) ? (1.0 - Cover) : Cover;
    }
    return Coverage;
}

float4 SampleSource(FUiFilterArgs Args, float2 UV)
{
    return SampleTexture2D(Args.SourceID, Args.SamplerIndex, UV * Args.SourceRect.xy + Args.SourceRect.zw);
}

// Separable Gaussian; the caller runs it twice, swinging TexelStep from horizontal to vertical.
float4 BlurAxis(FUiFilterArgs Args, float2 UV)
{
    const float Sigma = max(Args.Sigma, 1e-4);
    const float Denom = 1.0 / (2.0 * Sigma * Sigma);

    float4 Sum    = SampleSource(Args, UV);
    float  Weight = 1.0;

    for (uint i = 1u; i <= Args.TapCount; ++i)
    {
        const float W = exp(-float(i * i) * Denom);
        Sum += (SampleSource(Args, UV + Args.TexelStep.xy * float(i)) +
                SampleSource(Args, UV - Args.TexelStep.xy * float(i))) * W;
        Weight += 2.0 * W;
    }
    return Sum / Weight;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FUiFilterArgs Args = *Pass();

    // Ahead of any early return, so the quad's derivatives stay defined.
    const float Clip = ClipCoverage(Args, Input.Position.xy);

    if (Args.Kind == UI_FILTER_BLUR)
    {
        return BlurAxis(Args, Input.UV) * Clip;
    }

    if (Args.Kind == UI_FILTER_DROP_SHADOW)
    {
        // Only the shadow; the caller composites the unfiltered source back over it.
        const float Alpha = SampleSource(Args, Input.UV - Args.TexelStep.zw).a;
        return Args.Color * Alpha * Clip;
    }

    float4 Source = SampleSource(Args, Input.UV);

    if (Args.Kind == UI_FILTER_OPACITY)
    {
        return Source * Args.Opacity * Clip;
    }

    if (Args.Kind == UI_FILTER_MASK_IMAGE)
    {
        const float2 MaskUV = Input.UV * Args.SourceRect.xy + Args.SourceRect.zw;
        return Source * SampleTexture2D(Args.MaskID, Args.SamplerIndex, MaskUV).a * Clip;
    }

    if (Args.Kind == UI_FILTER_COLOR_MATRIX)
    {
        // Authored against straight alpha, so undo and redo the premultiply around it.
        const float Alpha = Source.a;
        const float3 Straight = (Alpha > 1e-5) ? Source.rgb / Alpha : float3(0.0);
        const float4 Filtered = mul(Args.ColorMatrix, float4(Straight, 1.0));
        return float4(saturate(Filtered.rgb) * Alpha, Alpha) * Clip;
    }

    return Source * Clip;
}
