#pragma once
#include "SceneGlobals.slang"
#include "MeshletGeometry.slang"

// Every VisID decode lives here, since the five shaders that read it must agree bit for bit.

// MeshletVisBuffer writes ((MeshletSlot << 7) | TriIndex) + 1, so a cleared 0 means no geometry.
#define VISBUFFER_TRI_BITS 7u
#define VISBUFFER_TRI_MASK 0x7Fu

struct FVisTriangle
{
    FMeshletRef Ref;
    uint3       Tri;
    uint        TriIndex;
    bool        bValid;
};

// One triangle per quad scores 1.0, and only depth survivors count, so occluded overdraw is not measured.
float QuadLaneEfficiency(FTexture2DUInt VisBuffer, int2 Texel)
{
    const int2 Base = Texel & ~1;

    uint Raw[4];
    Raw[0] = VisBuffer.Load(Base + int2(0, 0));
    Raw[1] = VisBuffer.Load(Base + int2(1, 0));
    Raw[2] = VisBuffer.Load(Base + int2(0, 1));
    Raw[3] = VisBuffer.Load(Base + int2(1, 1));

    uint Covered  = 0u;
    uint Distinct = 0u;

    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        if (Raw[i] == 0u)
        {
            continue;
        }
        Covered += 1u;

        // Equality means same triangle of the same draw, so each distinct value is one shading invocation.
        bool bSeen = false;
        [unroll]
        for (uint j = 0u; j < 4u; ++j)
        {
            bSeen = bSeen || (j < i && Raw[j] == Raw[i]);
        }
        if (!bSeen)
        {
            Distinct += 1u;
        }
    }

    if (Distinct == 0u)
    {
        return 1.0;
    }
    return float(Covered) / (4.0 * float(Distinct));
}

// Returns ~0u for background and for any entry this frame did not write.
uint ResolveVisInstance(uint VisRaw, uint DrawListCount)
{
    if (VisRaw == 0u || DrawListCount == 0u)
    {
        return ~0u;
    }

    const uint MeshletSlot = (VisRaw - 1u) >> VISBUFFER_TRI_BITS;
    if (MeshletSlot >= DrawListCount)
    {
        return ~0u;
    }

    const FMeshletDraw Draw = MeshletDrawList().Get(MeshletSlot);
    // The tag turns a slot never written this frame into background instead of a stale instance.
    if (!Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) || Draw.InstanceID >= Scene().CullData.InstanceNum)
    {
        return ~0u;
    }
    return Draw.InstanceID;
}

// Dense deferred slot for the pixel, or ~0u for background, terrain's 0 stamp and unbinned materials.
uint ResolveVisMaterialSlot(uint VisRaw, uint DrawListCount, GPUSpan<uint> SlotByMaterial)
{
    const uint InstanceID = ResolveVisInstance(VisRaw, DrawListCount);
    if (InstanceID == ~0u)
    {
        return ~0u;
    }

    const uint MaterialIndex = GetInstanceMaterialIndex(InstanceID);
    if (!SlotByMaterial.Contains(MaterialIndex))
    {
        return ~0u;
    }

    // Material instances share their master's slot and dispatch, and callers bound the ~0u of unbinned materials.
    return SlotByMaterial.Get(MaterialIndex);
}

// Every index is clamped because a speculated BDA load off a stale draw-list gap faults the device.
FVisTriangle ResolveVisTriangle(uint VisRaw, uint DrawListCount)
{
    FVisTriangle Out;
    Out.Tri      = uint3(0u, 0u, 0u);
    Out.TriIndex = 0u;
    Out.bValid   = false;

    const uint InstCount   = Scene().CullData.InstanceNum;
    const uint VisID       = VisRaw - 1u;
    const uint MeshletSlot = VisID >> VISBUFFER_TRI_BITS;
    const uint TriId       = VisID & VISBUFFER_TRI_MASK;

    bool bValid         = (VisRaw != 0u) && (DrawListCount != 0u) && (MeshletSlot < DrawListCount);
    const uint SafeSlot = bValid ? MeshletSlot : 0u;

    const FMeshletDraw Draw = MeshletDrawList().Get(SafeSlot);
    bValid = bValid && Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) && (Draw.InstanceID < InstCount);

    FMeshletRef Ref;
    Ref.InstanceID        = (Draw.InstanceID < InstCount) ? Draw.InstanceID : 0u;
    Ref.SkinnedVertexBase = Ref.Inst.SkinnedVertexBase;   // camera-side only; shadows never reach here
    // Clamping by both counts sends a stale instance on the null header to slot 0 rather than into a live mesh's array.
    const uint MeshletCount = max(min(Ref.Inst.MeshletTotalCount, Ref.Header.MeshletCount), 1u);
    Ref.GlobalMeshletIdx  = min(Draw.MeshletIndex, MeshletCount - 1u);
    Ref.M                 = loadAligned<16>(Ref.Header.Meshlets + Ref.GlobalMeshletIdx);

    Out.Ref      = Ref;
    Out.TriIndex = min(TriId, max(Ref.M.TriangleCount, 1u) - 1u);
    Out.Tri      = UnpackMeshletTriangle(Ref, Out.TriIndex);
    Out.bValid   = bValid;
    return Out;
}

// Perspective-correct barycentrics (Schied/Stachowiak), with DDX/DDY as d(lambda)/d(pixel).
struct FBaryDeriv
{
    float3 Lambda;
    float3 DDX;
    float3 DDY;
};

// Analytic because neighboring quad lanes in this full-screen pass can belong to different triangles.
float TriangleEdgeDistancePixels(FBaryDeriv Bary)
{
    const float3 Rate = float3(length(float2(Bary.DDX.x, Bary.DDY.x)),
                               length(float2(Bary.DDX.y, Bary.DDY.y)),
                               length(float2(Bary.DDX.z, Bary.DDY.z)));

    const float3 Dist = abs(Bary.Lambda) / max(Rate, float3(1e-8, 1e-8, 1e-8));
    return min(Dist.x, min(Dist.y, Dist.z));
}

FBaryDeriv CalcFullBary(float4 Pt0, float4 Pt1, float4 Pt2, float2 PixelNdc, float2 WinSize)
{
    FBaryDeriv Ret;
    float3 InvW = rcp(float3(Pt0.w, Pt1.w, Pt2.w));

    float2 Ndc0 = Pt0.xy * InvW.x;
    float2 Ndc1 = Pt1.xy * InvW.y;
    float2 Ndc2 = Pt2.xy * InvW.z;

    float InvDet = rcp(determinant(float2x2(Ndc2 - Ndc1, Ndc0 - Ndc1)));
    Ret.DDX = float3(Ndc1.y - Ndc2.y, Ndc2.y - Ndc0.y, Ndc0.y - Ndc1.y) * InvDet * InvW;
    Ret.DDY = float3(Ndc2.x - Ndc1.x, Ndc0.x - Ndc2.x, Ndc1.x - Ndc0.x) * InvDet * InvW;
    float DDXSum = dot(Ret.DDX, float3(1.0, 1.0, 1.0));
    float DDYSum = dot(Ret.DDY, float3(1.0, 1.0, 1.0));

    float2 DeltaVec  = PixelNdc - Ndc0;
    float InterpInvW = InvW.x + DeltaVec.x * DDXSum + DeltaVec.y * DDYSum;
    float InterpW    = rcp(InterpInvW);

    Ret.Lambda.x = InterpW * (InvW.x + DeltaVec.x * Ret.DDX.x + DeltaVec.y * Ret.DDY.x);
    Ret.Lambda.y = InterpW * (0.0    + DeltaVec.x * Ret.DDX.y + DeltaVec.y * Ret.DDY.y);
    Ret.Lambda.z = InterpW * (0.0    + DeltaVec.x * Ret.DDX.z + DeltaVec.y * Ret.DDY.z);

    Ret.DDX *= (2.0 / WinSize.x);
    Ret.DDY *= (2.0 / WinSize.y);
    DDXSum  *= (2.0 / WinSize.x);
    DDYSum  *= (2.0 / WinSize.y);

    // NDC y points opposite screen y.
    Ret.DDY *= -1.0;
    DDYSum  *= -1.0;

    float InterpWDDX = 1.0 / (InterpInvW + DDXSum);
    float InterpWDDY = 1.0 / (InterpInvW + DDYSum);

    Ret.DDX = InterpWDDX * (Ret.Lambda * InterpInvW + Ret.DDX) - Ret.Lambda;
    Ret.DDY = InterpWDDY * (Ret.Lambda * InterpInvW + Ret.DDY) - Ret.Lambda;

    return Ret;
}

// Passing DDX/DDY as the weights yields the attribute's exact screen derivative.
float3 InterpWeights(float3 Weights, float3 V0, float3 V1, float3 V2)
{
    return V0 * Weights.x + V1 * Weights.y + V2 * Weights.z;
}

float4 InterpWeights4(float3 Weights, float4 V0, float4 V1, float4 V2)
{
    return V0 * Weights.x + V1 * Weights.y + V2 * Weights.z;
}

float3 InterpBary(FBaryDeriv D, float3 V0, float3 V1, float3 V2)
{
    return InterpWeights(D.Lambda, V0, V1, V2);
}

float4 InterpBary4(FBaryDeriv D, float4 V0, float4 V1, float4 V2)
{
    return InterpWeights4(D.Lambda, V0, V1, V2);
}

// FinalWorldPos is post-WPO, so the caller must have run the material vertex graph on all three corners.
struct FVisSurface
{
    FBaryDeriv Bary;
    float3     WorldPosition;
    // The same point before the material graph displaced it; equal to WorldPosition without WPO.
    float3     WorldPositionNoOffset;
    float3     NormalWS;
    float4     TangentWS;
    float4     VertexColor;
    float2     UV;
    float2     UVDDX;
    float2     UVDDY;
    float2     UV1;
    float2     UV1DDX;
    float2     UV1DDY;
};

FVisSurface InterpolateVisSurface(FMeshletWorldVertex W[3], float3 FinalWorldPos[3],
                                  float2 PixelNdc, float2 ScreenSize)
{
    const float4 C0 = CameraClipPosition(CameraViewPosition(FinalWorldPos[0]));
    const float4 C1 = CameraClipPosition(CameraViewPosition(FinalWorldPos[1]));
    const float4 C2 = CameraClipPosition(CameraViewPosition(FinalWorldPos[2]));

    FVisSurface S;
    S.Bary          = CalcFullBary(C0, C1, C2, PixelNdc, ScreenSize);
    S.WorldPosition = InterpBary(S.Bary, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
    S.WorldPositionNoOffset = InterpBary(S.Bary, W[0].WorldPos, W[1].WorldPos, W[2].WorldPos);
    S.NormalWS      = InterpBary(S.Bary, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
    S.TangentWS     = float4(InterpBary(S.Bary, W[0].TangentWS.xyz, W[1].TangentWS.xyz, W[2].TangentWS.xyz),
                             W[0].TangentWS.w);
    S.VertexColor   = InterpBary4(S.Bary, W[0].Color, W[1].Color, W[2].Color);

    const float3 UVx = float3(W[0].UV.x, W[1].UV.x, W[2].UV.x);
    const float3 UVy = float3(W[0].UV.y, W[1].UV.y, W[2].UV.y);
    S.UV    = float2(dot(UVx, S.Bary.Lambda), dot(UVy, S.Bary.Lambda));
    S.UVDDX = float2(dot(UVx, S.Bary.DDX),    dot(UVy, S.Bary.DDX));
    S.UVDDY = float2(dot(UVx, S.Bary.DDY),    dot(UVy, S.Bary.DDY));

    // Set 1 needs its own derivatives since its texel density is independent of set 0's.
    const float3 UV1x = float3(W[0].UV1.x, W[1].UV1.x, W[2].UV1.x);
    const float3 UV1y = float3(W[0].UV1.y, W[1].UV1.y, W[2].UV1.y);
    S.UV1    = float2(dot(UV1x, S.Bary.Lambda), dot(UV1y, S.Bary.Lambda));
    S.UV1DDX = float2(dot(UV1x, S.Bary.DDX),    dot(UV1y, S.Bary.DDX));
    S.UV1DDY = float2(dot(UV1x, S.Bary.DDY),    dot(UV1y, S.Bary.DDY));
    return S;
}

// Pixel centers, matching where the rasterizer sampled coverage.
float2 VisPixelNdc(uint2 Texel, float2 ScreenSize)
{
    return ((float2(Texel) + 0.5) / ScreenSize) * 2.0 - 1.0;
}

// Ignores the material's vertex graph, so a WPO-driven triangle reconstructs slightly off.
FVisSurface ReconstructVisSurfaceNoWPO(FVisTriangle T, uint2 Texel, float2 ScreenSize)
{
    const float4x4 ModelMatrix    = T.Ref.ModelMatrix;
    const float3x3 NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

    FMeshletWorldVertex W[3];
    float3 WorldPos[3];
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        W[k]        = TransformMeshletVertex(LoadMeshletVertexData(T.Ref, T.Tri[k], true), ModelMatrix, NormalMatrixWS);
        WorldPos[k] = W[k].WorldPos;
    }

    return InterpolateVisSurface(W, WorldPos, VisPixelNdc(Texel, ScreenSize), ScreenSize);
}

// There is no SV_IsFrontFace outside the rasterizer, so two-sided back faces flip toward the camera here.
float3 OrientVisNormal(FMeshletRef Ref, float3 NormalWS, float3 WorldPosition)
{
    if (!Ref.InstHasFlag(EInstanceFlags::TwoSided))
    {
        return NormalWS;
    }

    const float3 ViewDir = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);
    return (dot(NormalWS, ViewDir) < 0.0) ? -NormalWS : NormalWS;
}

// 16 bits per axis, so a target wider than 65535 would alias two pixels onto one entry.
uint PackVisPixel(uint2 Texel)   { return (Texel.y << 16) | (Texel.x & 0xFFFFu); }
uint2 UnpackVisPixel(uint Packed) { return uint2(Packed & 0xFFFFu, Packed >> 16); }
