#pragma once
#include "Includes/RmlUiCommon.slang"

struct PSInput
{
    float4 Position : SV_Position;
    float4 Colour   : COLOR;
    float2 UV       : TEXCOORD0;
    nointerpolation uint DrawIndex : TEXCOORD1;
};

float StopPosition(uint Index)
{
    if (!Pass().Stops.Contains(Index))
    {
        return 0.0;
    }
    return Pass().Stops.At(Index).Position.x;
}

float4 StopColor(uint Index)
{
    if (!Pass().Stops.Contains(Index))
    {
        return float4(0.0);
    }
    return loadAligned<16>(&Pass().Stops.At(Index).Color);
}

// Stops before the one containing T saturate to weight 1 and stops after it to 0, an exact piecewise-linear ramp.
float4 SampleColorStops(FUiDraw Draw, float T)
{

    const uint Base  = Draw.StopOffset;
    const uint Count = Draw.StopCount;

    const float First = StopPosition(Base);
    const float Last  = StopPosition(Base + Count - 1u);

    if (Draw.bRepeating != 0u && Last > First)
    {
        const float Span = Last - First;
        T = First + (T - First) - Span * floor((T - First) / Span);
    }

    float4 Colour = StopColor(Base);
    for (uint i = 1u; i < Count; ++i)
    {
        const float P0 = StopPosition(Base + i - 1u);
        const float P1 = StopPosition(Base + i);
        const float W  = (P1 > P0) ? saturate((T - P0) / (P1 - P0)) : step(P1, T);
        Colour = lerp(Colour, StopColor(Base + i), W);
    }
    return Colour;
}

// RmlUi gives gradient geometry element-local pixel UVs, the same space as every shape parameter.
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

    // ShaderParams.zw is the start direction (angle 0 up), and its clockwise perpendicular (-y, x) gives the sweep axis.
    const float2 Up    = Draw.ShaderParams.zw;
    const float2 Right = float2(-Up.y, Up.x);
    const float2 D     = P - Draw.ShaderParams.xy;
    const float  Turns = atan2(dot(D, Right), dot(D, Up)) * (1.0 / 6.28318530718);
    return Turns - floor(Turns);
}

// Signed distance to a rounded box centered on the origin, negative inside.
float SdRoundedBox(float2 P, float2 HalfExtent, float4 Radii)
{
    float2 Pair   = (P.x > 0.0) ? Radii.yz : Radii.xw;   // right pair (TR, BR) or left pair (TL, BL)
    float  Radius = (P.y > 0.0) ? Pair.y : Pair.x;
    float2 Q      = abs(P) - HalfExtent + Radius;
    return min(max(Q.x, Q.y), 0.0) + length(max(Q, 0.0)) - Radius;
}

// Multiplied coverage instead of a discard keeps corners antialiased and intersects nested clips.
float ClipMaskCoverage(FUiDraw Draw, float2 FragCoord)
{
    const uint Count  = UI_CLIP_COUNT(Draw.ClipMaskRange);
    const uint Offset = UI_CLIP_OFFSET(Draw.ClipMaskRange);

    float Coverage = 1.0;
    for (uint i = 0u; i < Count; ++i)
    {
        FUiClipMask Mask = Pass().ClipMasks.Get(Offset + i);
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
    FUiDraw Draw = Pass().Draws.Get(Input.DrawIndex);

    // Ahead of any discard, because killing a lane leaves the quad's derivatives undefined.
    float Coverage = ClipMaskCoverage(Draw, Input.Position.xy);

    float2 P = Input.Position.xy;
    bool Scissored = P.x < Draw.ClipRect.x || P.y < Draw.ClipRect.y || P.x >= Draw.ClipRect.z || P.y >= Draw.ClipRect.w;

    if (Scissored || Coverage <= 0.0)
    {
        discard;
    }

    const bool bStraight = Draw.ShaderType == kUIShaderTexturedStraight || Draw.ShaderType == kUIShaderTexturedStraightSRGB;
    if (Draw.ShaderType != kUIShaderTextured && !bStraight)
    {
        return Input.Colour * SampleColorStops(Draw, GradientCoordinate(Draw, Input.UV)) * Coverage;
    }

    float4 Texel = Draw.Texture.Sample(Draw.Sampler, Input.UV);
    if (Draw.ShaderType == kUIShaderTexturedStraightSRGB)
    {
        Texel.rgb = EncodeSRGB(Texel.rgb);
    }
    if (bStraight)
    {
        Texel.rgb *= Texel.a;
    }
    return Input.Colour * Texel * Coverage;
}
