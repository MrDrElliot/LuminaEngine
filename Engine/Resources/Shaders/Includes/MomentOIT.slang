#pragma once
#include "SceneGlobals.slang"

// Moment-Based Order-Independent Transparency -- Münstermann et al., i3D 2018.
// https://momentsingraphics.de/I3D2018.html
//
// Absorbance -ln(T) is additive where transmittance is multiplicative, so a pixel's moments accumulate
// with plain One/One blending -- no sorting, no per-pixel lists. MomentGenerationPass accumulates,
// TransparentPass weights each fragment by the transmittance reconstructed at its own depth,
// OITResolvePass composites. Generation and shading must agree exactly on coverage and MomentWarpDepth.

// Four power moments, fp32 (R32F zeroth + RGBA32F). The paper's 16-bit path needs quantization matrices
// that are not ported here.
#define MOMENT_COUNT 4

static const float MOMENT_MAX_ABSORBANCE = 10.0;
static const float MOMENT_MIN_ABSORBANCE = 0.00100050033;

// Moments are nudged toward a neutral distribution before the solve. Without this the Hankel matrix is
// singular whenever every layer sits at one depth -- a single flat pane -- and the Cholesky divides by
// zero. Paper's single-precision 4-power-moment values.
static const float4 MOMENT_BIAS_VECTOR = float4(0.0, 0.375, 0.0, 0.375);
static const float  MOMENT_BIAS        = 5.0e-7;

// Fraction of its own absorbance a fragment sees in front of itself: a lone pane at alpha a is scaled by
// (1-a)^beta. The knob when translucency reads globally too dark or too washed out.
static const float MOMENT_OVERESTIMATION = 0.1;

// Linear view depth -> the [-1,1] moment domain. Logarithmic so four moments' fixed resolution lands
// where translucency clusters instead of spreading uniformly over [near, far].
float MomentWarpDepth(float ViewZ)
{
    float ZNear    = GetNearPlane();
    float ZFar     = GetFarPlane();
    float Z        = clamp(ViewZ, ZNear, ZFar);
    float LogRange = max(log(ZFar) - log(ZNear), 1e-6);

    return clamp(2.0 * (log(Z) - log(ZNear)) / LogRange - 1.0, -1.0, 1.0);
}

float MomentAbsorbance(float Coverage)
{
    float Transmittance = saturate(1.0 - Coverage);
    return min(-log(max(Transmittance, 1e-5)), MOMENT_MAX_ABSORBANCE);
}

void MomentGenerate(float WarpedDepth, float Absorbance, out float OutB0, out float4 OutB)
{
    float Z2 = WarpedDepth * WarpedDepth;

    OutB0 = Absorbance;
    OutB  = float4(WarpedDepth, Z2, Z2 * WarpedDepth, Z2 * Z2) * Absorbance;
}

// How much of this fragment survives everything in front of it. Fits the canonical distribution matching
// the moments (Hamburger), then integrates a step at the query depth against it. Caller must have
// checked B0 > MOMENT_MIN_ABSORBANCE.
float MomentTransmittanceAtDepth(float B0, float4 Moments, float WarpedDepth)
{
    float4 B = lerp(Moments / B0, MOMENT_BIAS_VECTOR, MOMENT_BIAS);

    // LDL^T of the Hankel matrix [[1, b1, b2], [b1, b2, b3], [b2, b3, b4]], non-trivial entries only.
    float L21D11 = mad(-B[0], B[1], B[2]);
    float D11    = mad(-B[0], B[0], B[1]);
    float InvD11 = 1.0 / D11;
    float L21    = L21D11 * InvD11;
    float D22    = mad(-L21D11, L21, mad(-B[1], B[1], B[3]));

    // Solve for the quadratic vanishing at the distribution's two support points.
    float3 C = float3(1.0, WarpedDepth, WarpedDepth * WarpedDepth);
    C[1] -= B[0];
    C[2] -= B[1] + L21 * C[1];
    C[1] *= InvD11;
    C[2] /= D22;
    C[1] -= L21 * C[2];
    C[0] -= dot(C.yz, B.xy);

    // max() only catches fp32 cancellation -- a NaN here poisons the whole pixel additively.
    float InvC2 = 1.0 / C[2];
    float P     = C[1] * InvC2;
    float Q     = C[0] * InvC2;
    float R     = sqrt(max(P * P * 0.25 - Q, 0.0));

    float3 Z = float3(WarpedDepth, -P * 0.5 - R, -P * 0.5 + R);

    // The warp is monotone in view depth, so a support point nearer than the query occludes it fully.
    float3 Weight = float3(MOMENT_OVERESTIMATION,
                           (Z[1] < Z[0]) ? 1.0 : 0.0,
                           (Z[2] < Z[0]) ? 1.0 : 0.0);

    float F01  = (Weight[1] - Weight[0]) / (Z[1] - Z[0]);
    float F12  = (Weight[2] - Weight[1]) / (Z[2] - Z[1]);
    float F012 = (F12 - F01) / (Z[2] - Z[0]);

    // Newton divided differences -> interpolating quadratic, monomial form.
    float3 Poly;
    Poly[0] = F012;
    Poly[1] = Poly[0];
    Poly[0] = F01 - Poly[0] * Z[1];
    Poly[2] = Poly[1];
    Poly[1] = Poly[0] - Poly[1] * Z[0];
    Poly[0] = Weight[0] - Poly[0] * Z[0];

    // Expected value under the biased measure = the absorbance in front of the query depth.
    float Absorbance = Poly[0] + dot(B.xy, Poly.yz);

    return saturate(exp(-B0 * Absorbance));
}

float MomentTotalTransmittance(float B0)
{
    return saturate(exp(-B0));
}
