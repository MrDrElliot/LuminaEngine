#pragma once
#include "SceneGlobals.slang"
#include "MeshletGeometry.slang"

// Everything the deferred lane needs to turn a visibility-buffer texel back into a surface: the fault-safe
// VisID -> meshlet/triangle resolve, the material-slot lookup the classify passes bin by, and the
// perspective-correct barycentric reconstruction with analytic screen derivatives.
//
// Five shaders decode the same VisID (classify count, classify scatter, the per-material GBuffer pass, the
// picker resolve and the debug-view pass). They MUST agree bit-for-bit or a pixel gets counted into one
// material's run and shaded by another's, so the decode lives here and is called, never re-derived.

// VisID packing, as written by MeshletVisBuffer / MeshletVisBufferVS: ((MeshletSlot << 7) | TriIndex) + 1.
// The +1 is what makes a cleared 0 mean "no geometry" rather than slot 0 triangle 0.
#define VISBUFFER_TRI_BITS 7u
#define VISBUFFER_TRI_MASK 0x7Fu

struct FVisTriangle
{
    FMeshletRef Ref;
    uint3       Tri;
    uint        TriIndex;
    bool        bValid;
};

// Resolves the draw-list slot without touching the meshlet header, for the passes that only need to know
// which material owns the pixel. Returns ~0u for background and for any entry this frame did not write.
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

    const FMeshletDraw Draw = MeshletDrawList()[MeshletSlot];
    // The tag turns "slot inside the allocation but never written this frame" from a silently resolved
    // stale instance into a background pixel.
    if (!Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) || Draw.InstanceID >= Scene().CullData.InstanceNum)
    {
        return ~0u;
    }
    return Draw.InstanceID;
}

// Dense deferred slot for the pixel, or ~0u when nothing shades it (background, terrain's 0 stamp, a
// material with no deferred shader, or a slot past MATERIAL_MAX_SLOTS).
uint ResolveVisMaterialSlot(uint VisRaw, uint DrawListCount, uint* SlotByMaterial, uint SlotByMaterialCount)
{
    const uint InstanceID = ResolveVisInstance(VisRaw, DrawListCount);
    if (InstanceID == ~0u)
    {
        return ~0u;
    }

    const uint MaterialIndex = GetInstance(InstanceID).MaterialIndex;
    if (MaterialIndex >= SlotByMaterialCount)
    {
        return ~0u;
    }

    // Every material instance of a master maps to the master's slot, so instances sharing a shader share
    // one dispatch; the shader still reads each pixel's own MaterialIndex for its uniforms.
    const uint Slot = SlotByMaterial[MaterialIndex];
    return (Slot < MATERIAL_MAX_SLOTS) ? Slot : ~0u;
}

// Full resolve down to the triangle's three local vertex indices. Every index is clamped rather than
// trusted: these feed buffer-device-address loads, and a speculated load off a stale draw-list gap faults
// the device instead of producing a wrong pixel.
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

    const FMeshletDraw Draw = MeshletDrawList()[SafeSlot];
    bValid = bValid && Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) && (Draw.InstanceID < InstCount);

    FMeshletRef Ref;
    Ref.InstanceID        = (Draw.InstanceID < InstCount) ? Draw.InstanceID : 0u;
    Ref.SkinnedVertexBase = Ref.Inst.SkinnedVertexBase;   // camera-side only; shadows never reach here
    const uint MeshletCount = max(Ref.Inst.MeshletTotalCount, 1u);
    Ref.GlobalMeshletIdx  = min(Draw.MeshletIndex, MeshletCount - 1u);
    Ref.M                 = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];

    Out.Ref      = Ref;
    Out.TriIndex = min(TriId, max(Ref.M.TriangleCount, 1u) - 1u);
    Out.Tri      = UnpackMeshletTriangle(Ref, Out.TriIndex);
    Out.bValid   = bValid;
    return Out;
}

// Perspective-correct barycentrics plus their screen-space derivatives, from the triangle's three
// clip-space positions and the pixel's NDC location (Schied/Stachowiak, "Visibility Buffer Rendering with
// Material Graphs"). DDX/DDY are d(lambda)/d(screen pixel), which is what lets any interpolated attribute
// be differentiated analytically -- no rasterizer ddx/ddy, so mips stay correct across triangle boundaries
// and the whole reconstruction is legal in a compute shader.
struct FBaryDeriv
{
    float3 Lambda;
    float3 DDX;
    float3 DDY;
};

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

// Interpolate three per-vertex values with an arbitrary weight vector. Passing Lambda gives the attribute;
// passing DDX/DDY gives its exact screen-space derivative, because d(sum Lambda_i * V_i)/dx is just the
// same sum taken with dLambda_i/dx. That is what lets a material graph build a UV out of world position and
// still pick the right mip.
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

// The reconstructed surface at one pixel. FinalWorldPos is post-WorldPositionOffset, so the caller has to
// have run the material's vertex graph on all three corners first -- that is what makes the reconstructed
// triangle the one the geometry pass actually rasterized.
struct FVisSurface
{
    FBaryDeriv Bary;
    float3     WorldPosition;
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
    S.NormalWS      = InterpBary(S.Bary, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
    S.TangentWS     = float4(InterpBary(S.Bary, W[0].TangentWS.xyz, W[1].TangentWS.xyz, W[2].TangentWS.xyz),
                             W[0].TangentWS.w);
    S.VertexColor   = InterpBary4(S.Bary, W[0].Color, W[1].Color, W[2].Color);

    const float3 UVx = float3(W[0].UV.x, W[1].UV.x, W[2].UV.x);
    const float3 UVy = float3(W[0].UV.y, W[1].UV.y, W[2].UV.y);
    S.UV    = float2(dot(UVx, S.Bary.Lambda), dot(UVy, S.Bary.Lambda));
    S.UVDDX = float2(dot(UVx, S.Bary.DDX),    dot(UVy, S.Bary.DDX));
    S.UVDDY = float2(dot(UVx, S.Bary.DDY),    dot(UVy, S.Bary.DDY));

    // Set 1 needs its own derivatives, not set 0's: the two are independent parameterizations, so a
    // shared gradient would pick the wrong mip wherever their texel densities differ.
    const float3 UV1x = float3(W[0].UV1.x, W[1].UV1.x, W[2].UV1.x);
    const float3 UV1y = float3(W[0].UV1.y, W[1].UV1.y, W[2].UV1.y);
    S.UV1    = float2(dot(UV1x, S.Bary.Lambda), dot(UV1y, S.Bary.Lambda));
    S.UV1DDX = float2(dot(UV1x, S.Bary.DDX),    dot(UV1y, S.Bary.DDX));
    S.UV1DDY = float2(dot(UV1x, S.Bary.DDY),    dot(UV1y, S.Bary.DDY));
    return S;
}

// Screen pixel -> the NDC location CalcFullBary expects. Pixel centers, matching where the rasterizer
// sampled coverage when it wrote this texel.
float2 VisPixelNdc(uint2 Texel, float2 ScreenSize)
{
    return ((float2(Texel) + 0.5) / ScreenSize) * 2.0 - 1.0;
}

// Reconstruction WITHOUT the material's vertex graph, for consumers that have no material shader to run:
// the debug-view pass, which needs the raw UV and the pre-normal-map geometric normal that the GBuffer does
// not carry. A material driving WorldPositionOffset displaces the triangle this does not displace, so its
// barycentrics differ slightly there -- acceptable in a diagnostic, and the reason the production material
// pass reconstructs with the token instead of calling this.
FVisSurface ReconstructVisSurfaceNoWPO(FVisTriangle T, uint2 Texel, float2 ScreenSize)
{
    const float4x4 ModelMatrix    = T.Ref.Inst.ModelMatrix;
    const float3x3 NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

    FMeshletWorldVertex W[3];
    float3 WorldPos[3];
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        W[k]        = TransformMeshletVertex(LoadMeshletVertexData(T.Ref, T.Tri[k]), ModelMatrix, NormalMatrixWS);
        WorldPos[k] = W[k].WorldPos;
    }

    return InterpolateVisSurface(W, WorldPos, VisPixelNdc(Texel, ScreenSize), ScreenSize);
}

// Two-sided materials rasterize back faces into the VisBuffer too (the geometry pass disables back-face
// culling for them). There is no SV_IsFrontFace outside the rasterizer, so flip the shading normal toward
// the camera; the tangent basis is re-orthogonalized against it downstream.
float3 OrientVisNormal(FMeshletRef Ref, float3 NormalWS, float3 WorldPosition)
{
    if (!Ref.Inst.HasFlag(EInstanceFlags::TwoSided))
    {
        return NormalWS;
    }

    const float3 ViewDir = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);
    return (dot(NormalWS, ViewDir) < 0.0) ? -NormalWS : NormalWS;
}

// The pixel list packs a screen position into one dword. 16 bits per axis, which the classify pass bounds
// against; a render target wider than 65535 would alias two pixels onto one entry.
uint PackVisPixel(uint2 Texel)   { return (Texel.y << 16) | (Texel.x & 0xFFFFu); }
uint2 UnpackVisPixel(uint Packed) { return uint2(Packed & 0xFFFFu, Packed >> 16); }
