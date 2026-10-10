#pragma once

// SMAA 1x (Enhanced Subpixel Morphological Antialiasing) port for Slang.
// Adapted from the reference implementation at https://github.com/iryoku/smaa
// Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia,
// Fernando Navarro, Diego Gutierrez. MIT licensed.

// The includer defines SMAA_RT_METRICS as float4(1/width, 1/height, width, height) first.

// The high preset (95% quality), each value overridable by defining it before the include.
#ifndef SMAA_THRESHOLD
#define SMAA_THRESHOLD 0.1
#endif
#ifndef SMAA_MAX_SEARCH_STEPS
#define SMAA_MAX_SEARCH_STEPS 16
#endif
#ifndef SMAA_MAX_SEARCH_STEPS_DIAG
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#endif
#ifndef SMAA_CORNER_ROUNDING
#define SMAA_CORNER_ROUNDING 25
#endif
#ifndef SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0
#endif

#define SMAA_AREATEX_MAX_DISTANCE       16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG  20
#define SMAA_AREATEX_PIXEL_SIZE         (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE        (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE             float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE      float2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM       (float(SMAA_CORNER_ROUNDING) / 100.0)

float SMAA_mad(float a, float b, float c) { return a * b + c; }
float2 SMAA_mad(float2 a, float2 b, float2 c) { return a * b + c; }
float3 SMAA_mad(float3 a, float3 b, float3 c) { return a * b + c; }
float4 SMAA_mad(float4 a, float4 b, float4 c) { return a * b + c; }

void SMAAMovc(bool2 cond, inout float2 variable, float2 value)
{
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
}

void SMAAMovc(bool4 cond, inout float4 variable, float4 value)
{
    SMAAMovc(cond.xy, variable.xy, value.xy);
    SMAAMovc(cond.zw, variable.zw, value.zw);
}

// Pairs a texture with its sampler, the combined object the reference SMAA helpers are written against.
struct SMAATex
{
    FTexture2D Tex;
    FSampler   Smp;
    float4 Sample(float2 uv)              { return Tex.Sample(Smp, uv); }
    float4 SampleLevel(float2 uv, float l) { return Tex.SampleLevel(Smp, uv, l); }
};

// Vertex-shader offset generators, called from the pixel shader here.
void SMAAEdgeDetectionOffsets(float2 texcoord, out float4 offset[3])
{
    offset[0] = SMAA_mad(SMAA_RT_METRICS.xyxy, float4(-1.0, 0.0, 0.0, -1.0), texcoord.xyxy);
    offset[1] = SMAA_mad(SMAA_RT_METRICS.xyxy, float4( 1.0, 0.0, 0.0,  1.0), texcoord.xyxy);
    offset[2] = SMAA_mad(SMAA_RT_METRICS.xyxy, float4(-2.0, 0.0, 0.0, -2.0), texcoord.xyxy);
}

void SMAABlendingWeightCalculationOffsets(float2 texcoord, out float2 pixcoord, out float4 offset[3])
{
    pixcoord = texcoord * SMAA_RT_METRICS.zw;

    offset[0] = SMAA_mad(SMAA_RT_METRICS.xyxy, float4(-0.25, -0.125,  1.25, -0.125), texcoord.xyxy);
    offset[1] = SMAA_mad(SMAA_RT_METRICS.xyxy, float4(-0.125, -0.25, -0.125,  1.25), texcoord.xyxy);

    offset[2] = SMAA_mad(SMAA_RT_METRICS.xxyy,
                         float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                         float4(offset[0].xz, offset[1].yw));
}

void SMAANeighborhoodBlendingOffsets(float2 texcoord, out float4 offset)
{
    offset = SMAA_mad(SMAA_RT_METRICS.xyxy, float4(1.0, 0.0, 0.0, 1.0), texcoord.xyxy);
}

float2 SMAALumaEdgeDetectionPS(float2 texcoord, float4 offset[3], SMAATex colorTex, float thresholdScalar)
{
    float2 threshold = float2(thresholdScalar, thresholdScalar);

    static const float3 weights = float3(0.2126, 0.7152, 0.0722);
    float L = dot(colorTex.Sample(texcoord).rgb, weights);

    float Lleft = dot(colorTex.Sample(offset[0].xy).rgb, weights);
    float Ltop  = dot(colorTex.Sample(offset[0].zw).rgb, weights);

    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    float Lright  = dot(colorTex.Sample(offset[1].xy).rgb, weights);
    float Lbottom = dot(colorTex.Sample(offset[1].zw).rgb, weights);
    delta.zw = abs(L - float2(Lright, Lbottom));

    float2 maxDelta = max(delta.xy, delta.zw);

    float Lleftleft = dot(colorTex.Sample(offset[2].xy).rgb, weights);
    float Ltoptop   = dot(colorTex.Sample(offset[2].zw).rgb, weights);
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

float2 SMAADecodeDiagBilinearAccess(float2 e)
{
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return round(e);
}

float4 SMAADecodeDiagBilinearAccess(float4 e)
{
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return round(e);
}

float2 SMAASearchDiag1(SMAATex edgesTex, float2 texcoord, float2 dir, out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    e = float2(0.0, 0.0);
    [loop] while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = SMAA_mad(t, float3(dir, 1.0), coord.xyz);
        e = edgesTex.SampleLevel(coord.xy, 0).rg;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAASearchDiag2(SMAATex edgesTex, float2 texcoord, float2 dir, out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * SMAA_RT_METRICS.x;
    float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    e = float2(0.0, 0.0);
    [loop] while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = SMAA_mad(t, float3(dir, 1.0), coord.xyz);
        e = edgesTex.SampleLevel(coord.xy, 0).rg;
        e = SMAADecodeDiagBilinearAccess(e);
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAAAreaDiag(SMAATex areaTex, float2 dist, float2 e, float offset)
{
    float2 texcoord = SMAA_mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);
    texcoord = SMAA_mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return areaTex.SampleLevel(texcoord, 0).rg;
}

float2 SMAACalculateDiagWeights(SMAATex edgesTex, SMAATex areaTex, float2 texcoord, float2 e, float4 subsampleIndices)
{
    float2 weights = float2(0.0, 0.0);

    float4 d;
    float2 end;
    if (e.r > 0.0)
    {
        d.xz = SMAASearchDiag1(edgesTex, texcoord, float2(-1.0,  1.0), end);
        d.x += float(end.y > 0.9);
    }
    else
    {
        d.xz = float2(0.0, 0.0);
    }
    d.yw = SMAASearchDiag1(edgesTex, texcoord, float2(1.0, -1.0), end);

    if (d.x + d.y > 2.0)
    {
        float4 coords = SMAA_mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.xy = edgesTex.SampleLevel(coords.xy + float2(-1.0,  0.0) * SMAA_RT_METRICS.xy, 0).rg;
        c.zw = edgesTex.SampleLevel(coords.zw + float2( 1.0,  0.0) * SMAA_RT_METRICS.xy, 0).rg;
        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);
        float2 cc = SMAA_mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.z);
    }

    d.xz = SMAASearchDiag2(edgesTex, texcoord, float2(-1.0, -1.0), end);
    if (edgesTex.SampleLevel(texcoord + float2(1.0, 0.0) * SMAA_RT_METRICS.xy, 0).r > 0.0)
    {
        d.yw = SMAASearchDiag2(edgesTex, texcoord, float2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    }
    else
    {
        d.yw = float2(0.0, 0.0);
    }

    if (d.x + d.y > 2.0)
    {
        float4 coords = SMAA_mad(float4(-d.x, -d.x, d.y, d.y), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.x  = edgesTex.SampleLevel(coords.xy + float2(-1.0,  0.0) * SMAA_RT_METRICS.xy, 0).g;
        c.y  = edgesTex.SampleLevel(coords.xy + float2( 0.0, -1.0) * SMAA_RT_METRICS.xy, 0).r;
        c.zw = edgesTex.SampleLevel(coords.zw + float2( 1.0,  0.0) * SMAA_RT_METRICS.xy, 0).gr;
        float2 cc = SMAA_mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}

float SMAASearchLength(SMAATex searchTex, float2 e, float offset)
{
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias  = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);

    scale += float2(-1.0,  1.0);
    bias  += float2( 0.5, -0.5);

    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias  *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;

    return searchTex.SampleLevel(SMAA_mad(scale, e, bias), 0).r;
}

float SMAASearchXLeft(SMAATex edgesTex, SMAATex searchTex, float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    [loop] while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0)
    {
        e = edgesTex.SampleLevel(texcoord, 0).rg;
        texcoord = SMAA_mad(-float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = SMAA_mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.0), 3.25);
    return SMAA_mad(SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchXRight(SMAATex edgesTex, SMAATex searchTex, float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    [loop] while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0)
    {
        e = edgesTex.SampleLevel(texcoord, 0).rg;
        texcoord = SMAA_mad(float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = SMAA_mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.5), 3.25);
    return SMAA_mad(-SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchYUp(SMAATex edgesTex, SMAATex searchTex, float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    [loop] while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0)
    {
        e = edgesTex.SampleLevel(texcoord, 0).rg;
        texcoord = SMAA_mad(-float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = SMAA_mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.0), 3.25);
    return SMAA_mad(SMAA_RT_METRICS.y, offset, texcoord.y);
}

float SMAASearchYDown(SMAATex edgesTex, SMAATex searchTex, float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    [loop] while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0)
    {
        e = edgesTex.SampleLevel(texcoord, 0).rg;
        texcoord = SMAA_mad(float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = SMAA_mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.5), 3.25);
    return SMAA_mad(-SMAA_RT_METRICS.y, offset, texcoord.y);
}

float2 SMAAArea(SMAATex areaTex, float2 dist, float e1, float e2, float offset)
{
    float2 texcoord = SMAA_mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE),
                               round(4.0 * float2(e1, e2)), dist);
    texcoord = SMAA_mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.y = SMAA_mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);
    return areaTex.SampleLevel(texcoord, 0).rg;
}

void SMAADetectHorizontalCornerPattern(SMAATex edgesTex, inout float2 weights, float4 texcoord, float2 d)
{
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * edgesTex.SampleLevel(texcoord.xy + float2(0.0,  1.0) * SMAA_RT_METRICS.xy, 0).r;
    factor.x -= rounding.y * edgesTex.SampleLevel(texcoord.zw + float2(1.0,  1.0) * SMAA_RT_METRICS.xy, 0).r;
    factor.y -= rounding.x * edgesTex.SampleLevel(texcoord.xy + float2(0.0, -2.0) * SMAA_RT_METRICS.xy, 0).r;
    factor.y -= rounding.y * edgesTex.SampleLevel(texcoord.zw + float2(1.0, -2.0) * SMAA_RT_METRICS.xy, 0).r;

    weights *= saturate(factor);
}

void SMAADetectVerticalCornerPattern(SMAATex edgesTex, inout float2 weights, float4 texcoord, float2 d)
{
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * edgesTex.SampleLevel(texcoord.xy + float2( 1.0, 0.0) * SMAA_RT_METRICS.xy, 0).g;
    factor.x -= rounding.y * edgesTex.SampleLevel(texcoord.zw + float2( 1.0, 1.0) * SMAA_RT_METRICS.xy, 0).g;
    factor.y -= rounding.x * edgesTex.SampleLevel(texcoord.xy + float2(-2.0, 0.0) * SMAA_RT_METRICS.xy, 0).g;
    factor.y -= rounding.y * edgesTex.SampleLevel(texcoord.zw + float2(-2.0, 1.0) * SMAA_RT_METRICS.xy, 0).g;

    weights *= saturate(factor);
}

float4 SMAABlendingWeightCalculationPS(float2 texcoord, float2 pixcoord, float4 offset[3],
                                       SMAATex edgesTex, SMAATex areaTex, SMAATex searchTex,
                                       float4 subsampleIndices)
{
    float4 weights = float4(0.0, 0.0, 0.0, 0.0);

    float2 e = edgesTex.Sample(texcoord).rg;

    if (e.g > 0.0)
    {
        weights.rg = SMAACalculateDiagWeights(edgesTex, areaTex, texcoord, e, subsampleIndices);

        if (weights.r == -weights.g)
        {
            float2 d;

            float3 coords;
            coords.x = SMAASearchXLeft(edgesTex, searchTex, offset[0].xy, offset[2].x);
            coords.y = offset[1].y;
            d.x = coords.x;

            float e1 = edgesTex.SampleLevel(coords.xy, 0).r;

            coords.z = SMAASearchXRight(edgesTex, searchTex, offset[0].zw, offset[2].y);
            d.y = coords.z;

            d = abs(round(SMAA_mad(SMAA_RT_METRICS.zz, d, -pixcoord.xx)));

            float2 sqrt_d = sqrt(d);

            float e2 = edgesTex.SampleLevel(coords.zy + float2(1.0, 0.0) * SMAA_RT_METRICS.xy, 0).r;

            weights.rg = SMAAArea(areaTex, sqrt_d, e1, e2, subsampleIndices.y);

            coords.y = texcoord.y;
            SMAADetectHorizontalCornerPattern(edgesTex, weights.rg, coords.xyzy, d);
        }
        else
        {
            e.r = 0.0;
        }
    }

    if (e.r > 0.0)
    {
        float2 d;

        float3 coords;
        coords.y = SMAASearchYUp(edgesTex, searchTex, offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;

        float e1 = edgesTex.SampleLevel(coords.xy, 0).g;

        coords.z = SMAASearchYDown(edgesTex, searchTex, offset[1].zw, offset[2].w);
        d.y = coords.z;

        d = abs(round(SMAA_mad(SMAA_RT_METRICS.ww, d, -pixcoord.yy)));

        float2 sqrt_d = sqrt(d);

        float e2 = edgesTex.SampleLevel(coords.xz + float2(0.0, 1.0) * SMAA_RT_METRICS.xy, 0).g;

        weights.ba = SMAAArea(areaTex, sqrt_d, e1, e2, subsampleIndices.x);

        coords.x = texcoord.x;
        SMAADetectVerticalCornerPattern(edgesTex, weights.ba, coords.xyxz, d);
    }

    return weights;
}

float4 SMAANeighborhoodBlendingPS(float2 texcoord, float4 offset, SMAATex colorTex, SMAATex blendTex)
{
    float4 a;
    a.x  = blendTex.Sample(offset.xy).a; // Right
    a.y  = blendTex.Sample(offset.zw).g; // Top
    a.wz = blendTex.Sample(texcoord).xz; // Bottom / Left

    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5)
    {
        return colorTex.SampleLevel(texcoord, 0);
    }
    else
    {
        bool h = max(a.x, a.z) > max(a.y, a.w);

        float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
        float2 blendingWeight = a.yw;
        SMAAMovc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0, a.z, 0.0));
        SMAAMovc(bool2(h, h), blendingWeight, a.xz);
        blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));

        float4 blendingCoord = SMAA_mad(blendingOffset, float4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy), texcoord.xyxy);

        float4 color  = blendingWeight.x * colorTex.SampleLevel(blendingCoord.xy, 0);
        color        += blendingWeight.y * colorTex.SampleLevel(blendingCoord.zw, 0);

        return color;
    }
}
