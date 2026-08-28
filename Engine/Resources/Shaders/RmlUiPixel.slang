// RmlUi pixel shader (fully-bindless, single-draw path). Per-draw data comes from the device-address
// buffer by flat draw index; scissor is an axis-aligned clip test here. Colors are premultiplied.
#pragma once
#include "Includes/RmlUiCommon.slang"

struct PSInput
{
    float4 Position : SV_Position;
    float4 Colour   : COLOR;
    float2 UV       : TEXCOORD0;
    nointerpolation uint DrawIndex : TEXCOORD1;
};

// Progressive lerp with a saturated segment weight: stops before the one containing T resolve to
// weight 1, stops after to 0, so this is an exact piecewise-linear ramp, not an approximation.
float4 SampleColorStops(FUiDraw Draw, float T)
{
    FUiColorStop* Stops = Pass().Stops;
    const uint Base  = Draw.StopOffset;
    const uint Count = Draw.StopCount;

    const float First = Stops[Base].Position.x;
    const float Last  = Stops[Base + Count - 1u].Position.x;

    if (Draw.bRepeating != 0u && Last > First)
    {
        const float Span = Last - First;
        T = First + (T - First) - Span * floor((T - First) / Span);
    }

    float4 Colour = Stops[Base].Color;
    for (uint i = 1u; i < Count; ++i)
    {
        const float P0 = Stops[Base + i - 1u].Position.x;
        const float P1 = Stops[Base + i].Position.x;
        const float W  = (P1 > P0) ? saturate((T - P0) / (P1 - P0)) : step(P1, T);
        Colour = lerp(Colour, Stops[Base + i].Color, W);
    }
    return Colour;
}

// RmlUi hands gradient geometry a UV equal to the vertex position minus the paint-area origin, and
// expresses every shape parameter in that same element-local pixel space.
float GradientCoordinate(FUiDraw Draw, float2 P)
{
    if (Draw.ShaderType == UI_SHADER_LINEAR)
    {
        const float2 P0 = Draw.ShaderParams.xy;
        return dot(P - P0, Draw.ShaderParams.zw - P0) * Draw.ShaderScale;
    }

    if (Draw.ShaderType == UI_SHADER_RADIAL)
    {
        const float2 Radius = max(abs(Draw.ShaderParams.zw), float2(1e-4));
        return length((P - Draw.ShaderParams.xy) / Radius);
    }

    // Conic: ShaderParams.zw is the start direction (angle 0 points up), and screen-space clockwise
    // rotation of (x, y) is (-y, x), so that vector's perpendicular gives the sweep axis.
    const float2 Up    = Draw.ShaderParams.zw;
    const float2 Right = float2(-Up.y, Up.x);
    const float2 D     = P - Draw.ShaderParams.xy;
    const float  Turns = atan2(dot(D, Right), dot(D, Up)) * (1.0 / 6.28318530718);
    return Turns - floor(Turns);
}

// Signed distance to a rounded box centred on the origin; negative inside.
float SdRoundedBox(float2 P, float2 HalfExtent, float4 Radii)
{
    float2 Pair   = (P.x > 0.0) ? Radii.yz : Radii.xw;   // right pair (TR, BR) or left pair (TL, BL)
    float  Radius = (P.y > 0.0) ? Pair.y : Pair.x;
    float2 Q      = abs(P) - HalfExtent + Radius;
    return min(max(Q.x, Q.y), 0.0) + length(max(Q, 0.0)) - Radius;
}

// Coverage rather than a hard discard, so the curve stays smooth at the corners. Nested clips
// intersect, and multiplying coverage is what intersection means once it is antialiased.
float ClipMaskCoverage(FUiDraw Draw, float2 FragCoord)
{
    const uint Count  = UI_CLIP_COUNT(Draw.ClipMaskRange);
    const uint Offset = UI_CLIP_OFFSET(Draw.ClipMaskRange);

    float Coverage = 1.0;
    for (uint i = 0u; i < Count; ++i)
    {
        FUiClipMask Mask = Pass().ClipMasks[Offset + i];
        float Distance   = SdRoundedBox(UiClipLocal(Mask, FragCoord) - Mask.Rect.xy, Mask.Rect.zw, Mask.Radii);

        // Distance is in mask space, so the screen-space derivative still converts it into target pixels.
        float Width = max(fwidth(Distance), 1e-5);
        float Cover = saturate(0.5 - Distance / Width);
        Coverage *= (Mask.Params.x != 0.0) ? (1.0 - Cover) : Cover;
    }
    return Coverage;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FUiDraw Draw = Pass().Draws[Input.DrawIndex];

    // Ahead of any discard, because killing a lane leaves the quad's derivatives undefined.
    float Coverage = ClipMaskCoverage(Draw, Input.Position.xy);

    // Shader-side scissor (axis-aligned, framebuffer pixels).
    float2 P = Input.Position.xy;
    bool Scissored = P.x < Draw.ClipRect.x || P.y < Draw.ClipRect.y || P.x >= Draw.ClipRect.z || P.y >= Draw.ClipRect.w;

    if (Scissored || Coverage <= 0.0)
    {
        discard;
    }

    if (Draw.ShaderType != UI_SHADER_TEXTURED)
    {
        return Input.Colour * SampleColorStops(Draw, GradientCoordinate(Draw, Input.UV)) * Coverage;
    }

    float4 Texel = SampleTexture2D(Draw.TextureID, Draw.SamplerIndex, Input.UV);
    return Input.Colour * Texel * Coverage;
}
