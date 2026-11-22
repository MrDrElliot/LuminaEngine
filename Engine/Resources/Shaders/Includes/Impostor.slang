#pragma once
#include "SceneGlobals.slang"

// Twin of Renderer/MeshImpostor.h, so the draw reads every view where the bake wrote it.

// The quad is grown past the sphere's radius so the parts in front of its center plane, which project larger, stay inside it.
static const float kImpostorQuadGrowth = 1.05;

// Blended coverage under which a near pixel shows what is behind the impostor, which far ones dither around.
static const float kImpostorCoverageCutoff = 0.5;

// Blended coverage under which a resolved impostor pixel has no surface of its own to shade.
static const float kImpostorMissCoverage = 0.02;

float3 ImpostorDecodeDirection(float2 GridUV)
{
    const float2 P = GridUV * 2.0 - 1.0;
    const float  X = (P.x + P.y) * 0.5;
    const float  Z = (P.x - P.y) * 0.5;
    return normalize(float3(X, max(1.0 - abs(X) - abs(Z), 0.0), Z));
}

// The inverse of ImpostorDecodeDirection, with a direction from below folded onto the horizon.
float2 ImpostorEncodeDirection(float3 Dir)
{
    const float3 D = float3(Dir.x, max(Dir.y, 0.0), Dir.z);
    const float3 P = D / max(abs(D.x) + D.y + abs(D.z), 1.0e-6);
    return float2(P.x + P.z, P.x - P.z) * 0.5 + 0.5;
}

void ImpostorFrameBasis(float3 Dir, out float3 Right, out float3 Up)
{
    const float K = 1.0 / (1.0 + Dir.y);
    Right = float3(1.0 - K * Dir.x * Dir.x, -Dir.x, -K * Dir.x * Dir.z);
    Up    = float3(K * Dir.x * Dir.z, Dir.z, K * Dir.z * Dir.z - 1.0);
}

// The three views around a direction and their weights, the grid cell split along its diagonal.
struct FImpostorViews
{
    uint2  Frame[3];
    float3 Weights;
};

FImpostorViews SelectImpostorViews(float3 LocalToEye, uint Frames)
{
    const float  Last = float(Frames - 1u);
    const float2 Grid = ImpostorEncodeDirection(LocalToEye) * Last;
    const float2 Cell = clamp(floor(Grid), 0.0, Last - 1.0);
    const float2 F    = Grid - Cell;
    const uint2  C    = uint2(Cell);

    FImpostorViews V;
    if (F.x + F.y < 1.0)
    {
        V.Frame[0] = C;
        V.Frame[1] = C + uint2(1u, 0u);
        V.Frame[2] = C + uint2(0u, 1u);
        V.Weights  = float3(1.0 - F.x - F.y, F.x, F.y);
    }
    else
    {
        V.Frame[0] = C + uint2(1u, 1u);
        V.Frame[1] = C + uint2(0u, 1u);
        V.Frame[2] = C + uint2(1u, 0u);
        V.Weights  = float3(F.x + F.y - 1.0, 1.0 - F.x, 1.0 - F.y);
    }
    V.Weights = saturate(V.Weights);
    return V;
}

// Where the mesh-local ray crosses the view's plane through the sphere's center, as a UV into that view's cell of the atlas and a distance along the ray.
float2 ImpostorAtlasUV(FMeshletHeader* Header, uint2 Frame, float3 RayOrigin, float3 RayDir, out float OutT, out float OutDenom)
{
    const uint   Frames  = Header.ImpostorFrames;
    const float3 ViewDir = ImpostorDecodeDirection(float2(Frame) / float(Frames - 1u));
    float3 Right, Up;
    ImpostorFrameBasis(ViewDir, Right, Up);

    const float  Denom  = dot(RayDir, ViewDir);
    const float  T      = dot(Header.ImpostorCenter - RayOrigin, ViewDir) / (abs(Denom) > 1.0e-6 ? Denom : -1.0e-6);
    const float3 Offset = RayOrigin + RayDir * T - Header.ImpostorCenter;
    OutT     = T;
    OutDenom = abs(Denom) > 1.0e-6 ? Denom : -1.0e-6;

    const float  HalfTexel = 0.5 / float(Header.ImpostorFrameSize);
    const float2 Local     = clamp(float2(dot(Offset, Right), dot(Offset, Up)) / (2.0 * Header.ImpostorRadius) + 0.5, HalfTexel, 1.0 - HalfTexel);
    return (float2(Frame) + Local) / float(Frames);
}

// The mip whose texel is about one screen pixel, from how large the sphere projects.
float ImpostorMipLevel(FMeshletHeader* Header, float3 WorldCenter, float WorldRadius)
{
    const float4x4 Proj    = GetCameraProjection();
    const float    Height  = float(GetScreenSize().y);
    const float    ViewZ   = max(mul(GetCameraView(), float4(WorldCenter, 1.0)).z, 1.0e-3);
    const float    Pixels  = IsCameraOrthographic() ? WorldRadius * abs(Proj[1][1]) * Height
                                                    : WorldRadius * abs(Proj[1][1]) * Height / ViewZ;
    return max(log2(float(Header.ImpostorFrameSize) / max(Pixels, 1.0e-3)), 0.0);
}

struct FImpostorSample
{
    float3 BaseColor;
    float  Coverage;
    // Mesh-local, unnormalized when the views disagree.
    float3 Normal;
    float  AmbientOcclusion;
    // How far along the mesh-local ray the baked surface lies.
    float  RayT;
};

// Each view weighted by its share and its coverage, so a view that misses the pixel does not darken the others.
FImpostorSample SampleImpostor(FMeshletHeader* Header, FImpostorViews Views, float3 RayOrigin, float3 RayDir, float Mip)
{
    const FTexture2D BaseColorAtlas   = FTexture2D(Header.ImpostorBaseColorIndex);
    const FTexture2D NormalDepthAtlas = FTexture2D(Header.ImpostorNormalDepthIndex);

    FImpostorSample S = (FImpostorSample)0;
    float3 Color  = float3(0.0, 0.0, 0.0);
    float3 Normal = float3(0.0, 0.0, 0.0);
    float  AO     = 0.0;
    float  RayT   = 0.0;
    float  PlaneT = 0.0;
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        float T, Denom;
        const float2 UV   = ImpostorAtlasUV(Header, Views.Frame[i], RayOrigin, RayDir, T, Denom);
        const float4 Base = BaseColorAtlas.SampleLevel(Samplers::LinearClamp, UV, Mip);
        const float4 ND   = NormalDepthAtlas.SampleLevel(Samplers::LinearClamp, UV, Mip);
        const float  W    = Views.Weights[i] * Base.a;
        S.Coverage += W;
        Color      += Base.rgb * W;
        Normal     += OctDecode01(ND.rg) * W;
        AO         += ND.a * W;
        // The bake stored the surface's offset toward the view as a share of the radius, which the ray reaches this much sooner.
        RayT       += (T + (ND.b * 2.0 - 1.0) * Header.ImpostorRadius / Denom) * W;
        PlaneT     += T * Views.Weights[i];
    }

    const float Inv = 1.0 / max(S.Coverage, 1.0e-4);
    S.BaseColor        = Color * Inv;
    S.Normal           = Normal;
    S.AmbientOcclusion = AO * Inv;
    S.RayT             = S.Coverage > 1.0e-4 ? RayT * Inv : PlaneT;
    return S;
}

// The camera ray through a world point in the instance's own space, and the direction from the impostor toward the eye that picks its views.
struct FImpostorRay
{
    float3 Origin;
    float3 Dir;
    float3 ToEye;
};

FImpostorRay MakeImpostorRay(FMeshletHeader* Header, float4x4 Model, float3 WorldPoint)
{
    const float4x4 WorldToLocal = MakeInstanceWorldToLocal(Model);
    const bool     bOrtho       = IsCameraOrthographic();
    const float3   WorldDir     = bOrtho ? GetCameraForward() : normalize(WorldPoint - GetCameraPosition());

    FImpostorRay R;
    R.Origin = mul(WorldToLocal, float4(WorldPoint, 1.0)).xyz;
    R.Dir    = normalize(mul((float3x3)WorldToLocal, WorldDir));
    R.ToEye  = bOrtho ? -R.Dir : normalize(mul(WorldToLocal, float4(GetCameraPosition(), 1.0)).xyz - Header.ImpostorCenter);
    return R;
}
