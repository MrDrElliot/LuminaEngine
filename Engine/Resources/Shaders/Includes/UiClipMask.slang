#pragma once

#define UI_CLIP_COUNT(Range)  ((Range) & 0xFFu)
#define UI_CLIP_OFFSET(Range) ((Range) >> 8u)

// An axis-aligned rounded rect in the clip element's own space, so an SDF replaces a stencil attachment.
struct FUiClipMask
{
    float4 Rect;         // center.xy, half-extent.zw in the mask's own space
    float4 Radii;        // top-left, top-right, bottom-right, bottom-left
    float4 Params;       // .x nonzero keeps the area outside the shape, .yz translate InvTransform
    float4 InvTransform; // row-major 2x2 taking a framebuffer pixel into the mask's own space
};

// RmlUi transforms each clip element separately, so the fragment moves into the mask rather than the reverse.
float2 UiClipLocal(FUiClipMask Mask, float2 FragCoord)
{
    return float2(dot(Mask.InvTransform.xy, FragCoord), dot(Mask.InvTransform.zw, FragCoord)) + Mask.Params.yz;
}

// Signed distance to a rounded box centered on the origin, negative inside.
float SdRoundedBox(float2 P, float2 HalfExtent, float4 Radii)
{
    float2 Pair   = (P.x > 0.0) ? Radii.yz : Radii.xw;   // right pair (TR, BR) or left pair (TL, BL)
    float  Radius = (P.y > 0.0) ? Pair.y : Pair.x;
    float2 Q      = abs(P) - HalfExtent + Radius;
    return min(max(Q.x, Q.y), 0.0) + length(max(Q, 0.0)) - Radius;
}

// Multiplied across masks instead of a discard, which keeps corners antialiased and intersects nested clips.
float UiClipMaskCoverage(FUiClipMask Mask, float2 FragCoord)
{
    const float Distance = SdRoundedBox(UiClipLocal(Mask, FragCoord) - Mask.Rect.xy, Mask.Rect.zw, Mask.Radii);

    // Distance is in mask space, so the screen-space derivative still converts it into target pixels.
    const float Width = max(fwidth(Distance), 1e-5);
    const float Cover = saturate(0.5 - Distance / Width);
    return (Mask.Params.x != 0.0) ? (1.0 - Cover) : Cover;
}
