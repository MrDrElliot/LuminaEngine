#pragma once
#include "SceneGlobals.slang"

// Twin of Renderer/MeshImpostor.h, so the draw reads every view where the bake wrote it.

// The quad is grown past the sphere's radius so the parts in front of its center plane, which project larger, stay inside it.
static const float kImpostorQuadGrowth = 1.05;

// How far a view may be stretched on screen before a denser mesh stops swapping sooner, the twin of Impostor::kMaxMagnification.
static const float kImpostorMaxMagnification = 1.5;

// The twin of Impostor::TransitionPixels, the projected diameter the mesh swaps to its impostor under.
float ImpostorTransitionPixels(FMeshletHeader* Header, float BasePixels)
{
    const float Scale  = Header.ImpostorTransitionScale;
    const float Scaled = BasePixels * Scale;
    return Scale > 1.0 ? min(Scaled, max(BasePixels, float(Header.ImpostorFrameSize) * kImpostorMaxMagnification)) : Scaled;
}

// Blended coverage under which a near pixel shows what is behind the impostor, which far ones dither around.
static const float kImpostorCoverageCutoff = 0.5;

// Blended coverage under which a resolved impostor pixel has no surface of its own to shade.
static const float kImpostorMissCoverage = 0.02;

// Y up. A hemisphere puts the horizon on the square's border, a full sphere folds the lower half into the corners.
float3 ImpostorDecodeDirection(float2 GridUV, bool bFullSphere)
{
    const float2 P = GridUV * 2.0 - 1.0;
    if (!bFullSphere)
    {
        const float X = (P.x + P.y) * 0.5;
        const float Z = (P.x - P.y) * 0.5;
        return normalize(float3(X, max(1.0 - abs(X) - abs(Z), 0.0), Z));
    }
    const float  Y     = 1.0 - abs(P.x) - abs(P.y);
    const float2 Signs = select(P >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
    const float2 XZ    = Y >= 0.0 ? P : (1.0 - abs(P.yx)) * Signs;
    return normalize(float3(XZ.x, Y, XZ.y));
}

// The inverse of ImpostorDecodeDirection, which on a hemisphere folds a direction from below onto the horizon.
float2 ImpostorEncodeDirection(float3 Dir, bool bFullSphere)
{
    if (!bFullSphere)
    {
        const float3 D = float3(Dir.x, max(Dir.y, 0.0), Dir.z);
        const float3 P = D / max(abs(D.x) + D.y + abs(D.z), 1.0e-6);
        return float2(P.x + P.z, P.x - P.z) * 0.5 + 0.5;
    }
    const float2 P     = Dir.xz / max(abs(Dir.x) + abs(Dir.y) + abs(Dir.z), 1.0e-6);
    const float2 Signs = select(P >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
    const float2 Folded = Dir.y >= 0.0 ? P : (1.0 - abs(P.yx)) * Signs;
    return Folded * 0.5 + 0.5;
}

// The twin of Impostor::FrameBasis, mirrored below the horizon so each view's axes stay a rotation.
void ImpostorFrameBasis(float3 Dir, out float3 Right, out float3 Up)
{
    if (Dir.y >= 0.0)
    {
        const float K = 1.0 / (1.0 + Dir.y);
        Right = float3(1.0 - K * Dir.x * Dir.x, -Dir.x, -K * Dir.x * Dir.z);
        Up    = float3(K * Dir.x * Dir.z, Dir.z, K * Dir.z * Dir.z - 1.0);
        return;
    }
    const float K = 1.0 / (1.0 - Dir.y);
    Right = float3(K * Dir.x * Dir.x - 1.0, -Dir.x, K * Dir.x * Dir.z);
    Up    = float3(K * Dir.x * Dir.z, -Dir.z, K * Dir.z * Dir.z - 1.0);
}

// The three views around a direction and their weights, the grid cell split along its diagonal.
struct FImpostorViews
{
    uint2  Frame[3];
    float3 Weights;
};

FImpostorViews SelectImpostorViews(FMeshletHeader* Header, float3 LocalToEye)
{
    const float  Last = float(Header.ImpostorFrames - 1u);
    const float2 Grid = ImpostorEncodeDirection(LocalToEye, Header.bImpostorFullSphere) * Last;
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
    const float3 ViewDir = ImpostorDecodeDirection(float2(Frame) / float(Frames - 1u), Header.bImpostorFullSphere);
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

// The three views' atlas taps and blended coverage, enough to reject a pixel before its normal and depth are fetched.
struct FImpostorTaps
{
    float2 UV[3];
    float  T[3];
    float  Denom[3];
    float4 Base[3];
    float  Coverage;
};

FImpostorTaps GatherImpostorTaps(FMeshletHeader* Header, FImpostorViews Views, float3 RayOrigin, float3 RayDir, float Mip)
{
    const FTexture2D BaseColorAtlas = FTexture2D(Header.ImpostorBaseColorIndex);

    FImpostorTaps Taps;
    Taps.Coverage = 0.0;
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        Taps.UV[i]     = ImpostorAtlasUV(Header, Views.Frame[i], RayOrigin, RayDir, Taps.T[i], Taps.Denom[i]);
        Taps.Base[i]   = BaseColorAtlas.SampleLevel(Samplers::LinearClamp, Taps.UV[i], Mip);
        Taps.Coverage += Views.Weights[i] * Taps.Base[i].a;
    }
    return Taps;
}

// Each view weighted by its share and its coverage, so a view that misses the pixel does not darken the others and skips its second fetch.
FImpostorSample ResolveImpostorTaps(FMeshletHeader* Header, FImpostorViews Views, FImpostorTaps Taps, float Mip)
{
    const FTexture2D NormalDepthAtlas = FTexture2D(Header.ImpostorNormalDepthIndex);

    FImpostorSample S = (FImpostorSample)0;
    S.Coverage    = Taps.Coverage;
    float3 Color  = float3(0.0, 0.0, 0.0);
    float3 Normal = float3(0.0, 0.0, 0.0);
    float  AO     = 0.0;
    float  RayT   = 0.0;
    float  PlaneT = 0.0;
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        const float W = Views.Weights[i] * Taps.Base[i].a;
        PlaneT += Taps.T[i] * Views.Weights[i];
        if (W > 0.0)
        {
            const float4 ND = NormalDepthAtlas.SampleLevel(Samplers::LinearClamp, Taps.UV[i], Mip);
            Color  += Taps.Base[i].rgb * W;
            Normal += OctDecode01(ND.rg) * W;
            AO     += ND.a * W;
            // The bake stored the surface's offset toward the view as a share of the radius, which the ray reaches this much sooner.
            RayT   += (Taps.T[i] + (ND.b * 2.0 - 1.0) * Header.ImpostorRadius / Taps.Denom[i]) * W;
        }
    }

    const float Inv = 1.0 / max(S.Coverage, 1.0e-4);
    S.BaseColor        = Color * Inv;
    S.Normal           = Normal;
    S.AmbientOcclusion = AO * Inv;
    S.RayT             = S.Coverage > 1.0e-4 ? RayT * Inv : PlaneT;
    return S;
}

// The views' emission at the taps, weighted like their base color, or none for a mesh baked without it.
float3 ResolveImpostorEmissive(FMeshletHeader* Header, FImpostorViews Views, FImpostorTaps Taps, float Mip)
{
    if (Header.ImpostorEmissiveIndex == 0u || Taps.Coverage <= 1.0e-4)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const FTexture2D EmissiveAtlas = FTexture2D(Header.ImpostorEmissiveIndex);
    float3 Emissive = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0u; i < 3u; ++i)
    {
        const float W = Views.Weights[i] * Taps.Base[i].a;
        if (W > 0.0)
        {
            // Stored as a square root, so dim emission keeps its steps in eight bits.
            const float3 Encoded = EmissiveAtlas.SampleLevel(Samplers::LinearClamp, Taps.UV[i], Mip).rgb;
            Emissive += Encoded * Encoded * W;
        }
    }
    return Emissive * (Header.ImpostorEmissiveScale / Taps.Coverage);
}

FImpostorSample SampleImpostor(FMeshletHeader* Header, FImpostorViews Views, float3 RayOrigin, float3 RayDir, float Mip)
{
    return ResolveImpostorTaps(Header, Views, GatherImpostorTaps(Header, Views, RayOrigin, RayDir, Mip), Mip);
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
