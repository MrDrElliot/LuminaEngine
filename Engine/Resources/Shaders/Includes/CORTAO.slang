#pragma once
#include "XeGTAO.slang"

// Slang port of CORTAO (https://github.com/theagentd/CORTAO), MIT license, copyright 2026 Daniel Isheden.

static const uint  CORTAO_RAY_COUNT    = 16u;

// Fewer than the reference's 10, because the bits replace depth precision that XeGTAO's center normals and screen space GI still read.
static const uint  CORTAO_PACKED_NORMAL_BITS = 8u;
static const uint  CORTAO_NORMAL_MASK        = (1u << CORTAO_PACKED_NORMAL_BITS) - 1u;

// The reference's CORTAOConfigurationParameters defaults.
static const float CORTAO_MAX_SCREEN_RADIUS_HEIGHT_RATIO = 0.2;
static const float CORTAO_MIP_BIAS                       = -4.0;
static const float CORTAO_MAX_SLOPE_COMPENSATION         = 4.0;
static const float CORTAO_BACKFACE_THRESHOLD             = 0.5;
static const float CORTAO_BACKFACE_BOOST                 = 0.5;

// How strongly the sample spiral crowds toward the center pixel.
static const float CORTAO_SPIRAL_SKEW = 3.5;

uint CORTAO_Hash(uint2 PixCoord, uint Seed)
{
    const uint2 Q = PixCoord * uint2(1719413u * 929u, 140473u * 2467u * 11u);
    const uint  N = (Q.x ^ Q.y) + Seed;
    return N * (N ^ (N >> 15u));
}

// Duff et al.'s branchless basis, since camera-facing normals here sit on the reference's Frisvad singularity at -z.
void CORTAO_Basis(float3 N, out float3 T, out float3 B)
{
    const float S = (N.z >= 0.0) ? 1.0 : -1.0;
    const float A = -1.0 / (S + N.z);
    const float C = N.x * N.y * A;
    T = float3(1.0 + S * N.x * N.x * A, S * C, -S * N.x);
    B = float3(C, S + N.y * N.y * A, -N.y);
}

// Spheremap around -z, the direction a visible surface faces in this view space.
uint CORTAO_EncodeNormal(float3 N, float2 Dither)
{
    const uint  Bits = CORTAO_PACKED_NORMAL_BITS / 2u;
    const uint  Mask = (1u << Bits) - 1u;
    const float Max  = float(Mask >> 1u);

    const float2 E = N.xy * rsqrt(max(2.0 - 2.0 * N.z, 1e-6));
    const int2   Q = int2(floor(E * Max + Dither));
    return (uint(Q.x) & Mask) | ((uint(Q.y) & Mask) << Bits);
}

float3 CORTAO_DecodeNormal(uint Packed)
{
    const uint  Bits = CORTAO_PACKED_NORMAL_BITS / 2u;
    const float Max  = float((1u << (Bits - 1u)) - 1u);

    const int2   Q = int2(int(Packed << (32u - Bits)), int(Packed << (32u - 2u * Bits))) >> int(32u - Bits);
    const float2 E = float2(Q) / Max;
    const float  Z = max(1.0 - dot(E, E), 0.0);
    return float3(E * (2.0 * sqrt(Z)), 1.0 - 2.0 * Z);
}

// The normal rides in depth's lowest mantissa bits, so the sample the main pass reads already carries it.
float CORTAO_PackDepthNormal(float Depth, float3 Normal, float2 Dither)
{
    return asfloat((asuint(XeGTAO_ClampDepth(Depth)) & ~CORTAO_NORMAL_MASK) | CORTAO_EncodeNormal(Normal, Dither));
}

// A filtered mip keeps the normal of the input whose depth it landed closest to.
float CORTAO_PackFilteredDepth(float Filtered, float4 Depths)
{
    const float4 Delta   = abs(Depths - Filtered);
    const float  Nearest = (Delta.x <= min(Delta.y, min(Delta.z, Delta.w))) ? Depths.x
                         : (Delta.y <= min(Delta.z, Delta.w))                ? Depths.y
                         : (Delta.z <= Delta.w)                              ? Depths.z
                         :                                                     Depths.w;
    return asfloat((asuint(XeGTAO_ClampDepth(Filtered)) & ~CORTAO_NORMAL_MASK) | (asuint(Nearest) & CORTAO_NORMAL_MASK));
}

float3 CORTAO_RayDirection(uint Ray)
{
    const float A = float(Ray) * kVogelGoldenAngle;
    const float R = sqrt((float(Ray) + 0.5) / float(CORTAO_RAY_COUNT));
    return float3(cos(A) * R, sin(A) * R, sqrt(1.0 - R * R));
}

// Bit per ray of the tangent-space hemisphere that a circular occluder of angular Radius in Direction covers.
uint CORTAO_OccludedRays(float3 Direction, float Radius)
{
    const float CosAngle = max(0.0, 1.0 - Radius * Radius * 0.5);

    uint Bits = 0u;
    [unroll]
    for (uint Ray = 0u; Ray < CORTAO_RAY_COUNT; ++Ray)
    {
        Bits |= (dot(CORTAO_RayDirection(Ray), Direction) > CosAngle) ? (1u << Ray) : 0u;
    }
    return Bits;
}

#ifdef CORTAO_SAMPLE_COUNT
// Visibility of the hemisphere around Normal at viewspace Position, out to Radius, from a pyramid of depths packed with normals.
float CORTAO_Visibility(FGTAOView V, FTexture2D PackedDepth, float3 Position, float3 Normal, float Radius, float2 Noise, uint Hash)
{
    const float3 ViewDir = normalize(-Position);
    const float  Z       = Position.z;

    // The sphere's silhouette is wider than its radius up close, so the sampled disc widens to cover it.
    const float MaxApparent    = CORTAO_MAX_SCREEN_RADIUS_HEIGHT_RATIO * abs(V.NDCToViewMul.y);
    const float ClampedRadius  = min(Radius, Z * MaxApparent * rsqrt(MaxApparent * MaxApparent + 1.0));
    const float ApparentRadius = ClampedRadius * rsqrt(Z * Z - ClampedRadius * ClampedRadius);
    const float DiscRadius     = ApparentRadius * Z;

    // Off-axis pixels see the disc stretched on screen, so they step to a coarser mip.
    const float BaseMip = log2(ApparentRadius * rsqrt(abs(ViewDir.z)) / abs(V.NDCToViewMulPixelSize.y)) + CORTAO_MIP_BIAS;

    const float3 Center = Position + Normal * (0.70710678 * abs(V.NDCToViewMulPixelSize.x) * Z);

    float3 Right, Up;
    CORTAO_Basis(ViewDir, Right, Up);
    // Alternating the spiral's winding per pixel cancels part of its bias.
    if ((Hash & 1u) != 0u)
    {
        Right = -Right;
    }

    const float  SpinAngle = Noise.x * (2.0 * XE_GTAO_PI);
    const float2 Spin      = float2(cos(SpinAngle), sin(SpinAngle)) * DiscRadius;

    float3 Tangent, Bitangent;
    CORTAO_Basis(Normal, Tangent, Bitangent);
    const float  TwistAngle = float(Hash >> 1u) * (2.0 * XE_GTAO_PI / 2147483648.0);
    const float3 RayX       = cos(TwistAngle) * Tangent + sin(TwistAngle) * Bitangent;
    const float3 RayY       = cos(TwistAngle) * Bitangent - sin(TwistAngle) * Tangent;

    uint RayBits = 0u;
    [unroll]
    for (uint Sample = 0u; Sample < CORTAO_SAMPLE_COUNT; ++Sample)
    {
        const float  Angle  = float(Sample) * kVogelGoldenAngle;
        const float  Inner  = pow(float(Sample) / float(CORTAO_SAMPLE_COUNT), CORTAO_SPIRAL_SKEW * 0.5);
        const float  Outer  = pow(float(Sample + 1u) / float(CORTAO_SAMPLE_COUNT), CORTAO_SPIRAL_SKEW * 0.5);
        // Each sample stands for the ring it is dithered across, and the weight scales a radius, not an area.
        const float  Weight = sqrt(Outer * Outer - Inner * Inner);
        const float2 Offset = float2(cos(Angle), sin(Angle)) * lerp(Inner, Outer, Noise.y);

        const float2 Disc     = RotateVogel(Offset, Spin);
        const float3 DiscPos  = Center + Right * Disc.x + Up * Disc.y;
        const float2 SampleUV = (DiscPos.xy / DiscPos.z - V.NDCToViewAdd) / V.NDCToViewMul;
        // The reference's border sampler reads off-screen as sky, which never occludes.
        if (any(SampleUV != saturate(SampleUV)))
        {
            continue;
        }

        const float  Packed = PackedDepth.SampleLevel(Samplers::PointClamp, SampleUV, BaseMip + log2(Outer)).x;
        const float3 Delta  = XeGTAO_ComputeViewspacePosition(V, SampleUV, Packed) - Center;
        const float  LenSq  = dot(Delta, Delta);
        if (LenSq >= ClampedRadius * ClampedRadius || dot(Delta, Normal) <= 0.0)
        {
            continue;
        }

        const float  InvLen       = rsqrt(LenSq);
        float3       Direction    = Delta * InvLen;
        const float3 SampleNormal = CORTAO_DecodeNormal(asuint(Packed));

        // A surface seen edge-on covers fewer pixels than it occludes, so its occluder grows to compensate.
        const float SlopeCompensation = rsqrt(max(dot(SampleNormal, ViewDir), 1.0 / CORTAO_MAX_SLOPE_COMPENSATION));
        float       OccluderRadius    = ClampedRadius * Weight * SlopeCompensation * InvLen;

        if (dot(SampleNormal, Direction) < CORTAO_BACKFACE_THRESHOLD)
        {
            // Sinking a front face one radius into its surface stops it occluding the plane it shares with the center.
            const float Sink = -OccluderRadius / max(1.0 - OccluderRadius * OccluderRadius * 0.5, 1e-6);
            Direction = normalize(Direction + SampleNormal * Sink);
        }
        else
        {
            // A back face hides an unknown thickness behind it, so it is grown to stop light leaking through thin walls.
            OccluderRadius *= lerp(1.0, SlopeCompensation, CORTAO_BACKFACE_BOOST);
        }

        RayBits |= CORTAO_OccludedRays(float3(dot(Direction, RayX), dot(Direction, RayY), dot(Direction, Normal)), OccluderRadius);
    }

    return 1.0 - float(countbits(RayBits)) / float(CORTAO_RAY_COUNT);
}
#endif
