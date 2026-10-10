#pragma once
#include "Includes/ParallaxOcclusion.slang"   // BuildTangentFrame, so the normal round trip matches GetWorldNormal

// Triplanar projection for the material graph's TriplanarSample node. Every fetch takes explicit gradients:
// the per-plane skip below is non-uniform control flow, where implicit derivatives are undefined, and
// SampleGrad is legal in every stage (the vertex lane passes zero gradients, which is mip 0).

static const float TRIPLANAR_PLANE_EPSILON = 1e-3;

struct FTriplanarPlanes
{
    float3 Weights;
    float2 UVX, UVY, UVZ;
    float2 DdxX, DdyX, DdxY, DdyY, DdxZ, DdyZ;
};

// Planes are zy / xz / xy, mirrored on negative-facing axes so the projection does not flip. The whiteout
// swizzles in TriplanarSampleNormal assume exactly these conventions -- change one, change both.
FTriplanarPlanes TriplanarProject(float3 P, float3 PDdx, float3 PDdy, float3 N, float Tiling, float Sharpness)
{
    FTriplanarPlanes Planes;

    float3 W = pow(abs(N), max(Sharpness, 1.0));
    Planes.Weights = W / max(W.x + W.y + W.z, 1e-5);

    // A zero component means that plane's weight is zero too, so sign() returning 0 is harmless.
    float3 S = sign(N);

    Planes.UVX  = float2(P.z * S.x, P.y) * Tiling;
    Planes.UVY  = float2(P.x * S.y, P.z) * Tiling;
    Planes.UVZ  = float2(P.x * -S.z, P.y) * Tiling;
    Planes.DdxX = float2(PDdx.z * S.x, PDdx.y) * Tiling;
    Planes.DdyX = float2(PDdy.z * S.x, PDdy.y) * Tiling;
    Planes.DdxY = float2(PDdx.x * S.y, PDdx.z) * Tiling;
    Planes.DdyY = float2(PDdy.x * S.y, PDdy.z) * Tiling;
    Planes.DdxZ = float2(PDdx.x * -S.z, PDdx.y) * Tiling;
    Planes.DdyZ = float2(PDdy.x * -S.z, PDdy.y) * Tiling;
    return Planes;
}

float4 TriplanarSample(uint TextureIndex, uint SamplerIndex, float3 P, float3 PDdx, float3 PDdy, float3 N,
                       float Tiling, float Sharpness)
{
    FTriplanarPlanes Planes = TriplanarProject(P, PDdx, PDdy, normalize(N), Tiling, Sharpness);

    float4 Result = float4(0.0, 0.0, 0.0, 0.0);
    if (Planes.Weights.x > TRIPLANAR_PLANE_EPSILON)
        Result += SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVX, Planes.DdxX, Planes.DdyX) * Planes.Weights.x;
    if (Planes.Weights.y > TRIPLANAR_PLANE_EPSILON)
        Result += SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVY, Planes.DdxY, Planes.DdyY) * Planes.Weights.y;
    if (Planes.Weights.z > TRIPLANAR_PLANE_EPSILON)
        Result += SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVZ, Planes.DdxZ, Planes.DdyZ) * Planes.Weights.z;
    return Result;
}

// Whiteout-blended normal map, returned as what the Normal pin expects: tangent space, encoded to [0,1].
float4 TriplanarSampleNormal(uint TextureIndex, uint SamplerIndex, float3 P, float3 PDdx, float3 PDdy, float3 N,
                             float4 Tangent, float Tiling, float Sharpness)
{
    float3 Nw = normalize(N);
    FTriplanarPlanes Planes = TriplanarProject(P, PDdx, PDdy, Nw, Tiling, Sharpness);

    float2 Tx = float2(0.0, 0.0);
    float2 Ty = float2(0.0, 0.0);
    float2 Tz = float2(0.0, 0.0);
    if (Planes.Weights.x > TRIPLANAR_PLANE_EPSILON)
        Tx = SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVX, Planes.DdxX, Planes.DdyX).xy * 2.0 - 1.0;
    if (Planes.Weights.y > TRIPLANAR_PLANE_EPSILON)
        Ty = SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVY, Planes.DdxY, Planes.DdyY).xy * 2.0 - 1.0;
    if (Planes.Weights.z > TRIPLANAR_PLANE_EPSILON)
        Tz = SampleTexture2DGrad(TextureIndex, SamplerIndex, Planes.UVZ, Planes.DdxZ, Planes.DdyZ).xy * 2.0 - 1.0;

    // Z is rebuilt from XY so BC5 maps (no blue channel) work too.
    float3 Nx = float3(Tx, sqrt(saturate(1.0 - dot(Tx, Tx))));
    float3 Ny = float3(Ty, sqrt(saturate(1.0 - dot(Ty, Ty))));
    float3 Nz = float3(Tz, sqrt(saturate(1.0 - dot(Tz, Tz))));

    Nx = float3(Nx.xy + Nw.zy, abs(Nx.z) * Nw.x);
    Ny = float3(Ny.xy + Nw.xz, abs(Ny.z) * Nw.y);
    Nz = float3(Nz.xy + Nw.xy, abs(Nz.z) * Nw.z);

    float3 NormalWS = normalize(Nx.zyx * Planes.Weights.x + Ny.xzy * Planes.Weights.y + Nz.xyz * Planes.Weights.z + 1e-6);

    // GetWorldNormal multiplies the pin by this frame, so the inverse (its transpose) brings the blend back.
    bool bDegenerate;
    float3x3 TBN = BuildTangentFrame(Nw, Tangent, bDegenerate);
    if (bDegenerate)
        return float4(0.5, 0.5, 1.0, 1.0);

    float3 NormalTS = normalize(mul(TBN, NormalWS));
    return float4(NormalTS * 0.5 + 0.5, 1.0);
}
