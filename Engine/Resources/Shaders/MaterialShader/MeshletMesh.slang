#pragma once
#ifndef MESHLET_MESH_BASE
#define MESHLET_SHADOW_PAGE_CLIP
#endif
#include "Includes/MeshletGeometry.slang"
#include "Includes/VirtualShadow.slang"

// Compiled per output type via MESHLET_MESH_BASE, since a spec constant cannot change output types.

// Masked shadow casters carry what their opacity graph reads, but rastered from the light rather than the camera.
#if defined(MESHLET_MESH_BASE)
typedef FBaseVertexOutput      FMeshVertexOutput;
#elif defined(MESHLET_MESH_MASKED_SHADOW)
typedef FMaskedVertexOutput    FMeshVertexOutput;
#else
typedef FDepthOnlyVertexOutput FMeshVertexOutput;
#endif


// Rejected in hardware rather than compacted, like FVisPrimitive in MeshletVisBuffer.slang.
struct FMeshPrimitiveOut
{
    bool CullPrimitive : SV_CullPrimitive;
#ifndef MESHLET_MESH_BASE
    // A multi-view draw sets one viewport and scissor per view on its atlas tile, in draw order.
    uint ViewportIndex : SV_ViewportArrayIndex;
#endif
};

// Kept outside the loop because the token expands raw statements that would otherwise be emitted per iteration.
FMaterialVertexInputs EvalMaterialVertex(FGPUInstance* Inst, float4x4 ModelMatrix, FVertexData VertexData,
                                         float4 WorldPos, float3 NormalWS, float3 TangentWS,
                                         float TangentSignWS)
{
    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    return Material;
}

// What every vertex of one meshlet reads alike, loaded once per mesh workgroup and once per vertex on the vertex path.
struct FMeshVertexContext
{
    float4x4 ModelMatrix;
    float3x3 NormalMatrix;
#if defined(MESHLET_MESH_BASE) || defined(MESHLET_MESH_MASKED_SHADOW)
    float4x4 CameraView;
    float4x4 CameraProj;
#endif
#ifdef MESHLET_MESH_MASKED_SHADOW
    uint     MaterialIndex;
#endif
#ifndef MESHLET_MESH_BASE
    float4x4 ShadowViewProj;
#endif
    bool     bApplyWPO;
};

FMeshVertexContext LoadMeshVertexContext(FMeshletRef Ref, uint DrawIndex)
{
    FMeshVertexContext Ctx;
    Ctx.ModelMatrix  = Ref.ModelMatrix;
    Ctx.NormalMatrix = CalculateNormalMatrix(Ctx.ModelMatrix);
#if defined(MESHLET_MESH_BASE) || defined(MESHLET_MESH_MASKED_SHADOW)
    Ctx.CameraView   = GetCameraView();
    Ctx.CameraProj   = GetCameraProjection();
#endif
#ifdef MESHLET_MESH_MASKED_SHADOW
    Ctx.MaterialIndex = Ref.Inst.MaterialIndex;
#endif
#ifndef MESHLET_MESH_BASE
    const FShadowRasterView ShadowView = ShadowRasterViewForDraw(DrawIndex);
    Ctx.ShadowViewProj = GetShadowViewProjection(ShadowView.ShadowDataIndex, (uint)ShadowView.ViewIndex);
    Ctx.bApplyWPO      = Ref.Inst.bApplyShadowWPO;
#else
    Ctx.bApplyWPO      = Ref.Inst.bApplyWPO;
#endif
    return Ctx;
}

#ifndef MESHLET_MESH_BASE
// The vertex path and every view other than a virtual level leave the page planes open.
static const float kOpenPageClip[4] = { 1.0, 1.0, 1.0, 1.0 };
#endif

// Both entry points emit their vertices through this, so the mesh and vertex paths place geometry identically.
FMeshVertexOutput MeshPassVertex(FMeshletRef Ref, uint LocalVertex, FMeshVertexContext Ctx)
{
    FVertexData VertexData = LoadMeshletVertexData(Ref, LocalVertex);
    FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, Ctx.ModelMatrix, Ctx.NormalMatrix);

    float4 WorldPos = float4(W.WorldPos, 1.0);
    if (Ctx.bApplyWPO)
    {
        const FMaterialVertexInputs Material =
            EvalMaterialVertex(Ref.Inst, Ctx.ModelMatrix, VertexData, WorldPos,
                               W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
        WorldPos.xyz += Material.WorldPositionOffset;
    }

    const float3 FinalWorldPos = WorldPos.xyz;

#ifdef MESHLET_MESH_BASE
    return MakeBaseVertexOutput(Ref, W, FinalWorldPos, Ctx.CameraView, Ctx.CameraProj);
#elif defined(MESHLET_MESH_MASKED_SHADOW)
    // Interpolants use the camera basis for the opacity graph, and only the clip position is the light's.
    FMeshVertexOutput VOut = MakeMaskedVertexOutput(Ref, W, FinalWorldPos, Ctx.CameraView, Ctx.CameraProj, Ctx.MaterialIndex);
    VOut.Position = mul(Ctx.ShadowViewProj, float4(FinalWorldPos, 1.0));
    VOut.PageClip = kOpenPageClip;
    return VOut;
#else
    FMeshVertexOutput VOut = (FMeshVertexOutput)0;
    VOut.Position = mul(Ctx.ShadowViewProj, float4(FinalWorldPos, 1.0));
    VOut.PageClip = kOpenPageClip;
    return VOut;
#endif
}

#ifndef MESHLET_MESH_BASE
// Mask cells per side the page rect scan reads, two per lane.
static const uint kPageRectScanSide = 8u;

// Shuffles across the workgroup, which the mesh stage pins to one subgroup.
float2 WorkgroupMin(float2 V)
{
    [ForceUnroll]
    for (uint Step = 1u; Step < MESHLET_MESH_GROUP_SIZE; Step <<= 1u)
    {
        V = min(V, WaveReadLaneAt(V, WaveGetLaneIndex() ^ Step));
    }
    return V;
}

float2 WorkgroupMax(float2 V)
{
    return -WorkgroupMin(-V);
}

// The NDC rect of drawn pages under a virtual level meshlet as (MinX, MinY, MaxX, MaxY), open for other views and inverted when no page under it draws.
float4 VirtualShadowMeshletPageRect(uint CullViewIndex, float4 Clip[MESHLET_VERT_ITERS], uint VertCount, uint ThreadID)
{
    const float4 Open  = float4(-2.0, -2.0, 2.0, 2.0);
    const float4 Empty = float4(1.0, 1.0, -1.0, -1.0);
    const FCullView View = CullViews().GetAligned(CullViewIndex);
    const GPUSpan<uint> Masks = GetSceneRoot().VirtualShadowMasks;
    if (!View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW) || Masks.IsEmpty())
    {
        return Open;
    }

    // A level is orthographic, so clip xy is already NDC.
    float2 Lo = float2(1.0e30, 1.0e30);
    float2 Hi = float2(-1.0e30, -1.0e30);
    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        if (ThreadID + v * MESHLET_MESH_GROUP_SIZE < VertCount)
        {
            Lo = min(Lo, Clip[v].xy);
            Hi = max(Hi, Clip[v].xy);
        }
    }
    Lo = WorkgroupMin(Lo);
    Hi = WorkgroupMax(Hi);
    if (any(Hi < -1.0) || any(Lo >= 1.0))
    {
        return Empty;
    }

    const FVirtualShadowLevel Lv = VirtualShadowLevel(View.ShadowLevel);
    const float  Size   = float(VSM_VIRTUAL_SIZE);
    const int2   PageLo = int2(clamp((Lo * 0.5 + 0.5) * Size, 0.0, Size - 1.0)) / int(VSM_PAGE_SIZE) + Lv.PageOrigin;
    const int2   PageHi = int2(clamp((Hi * 0.5 + 0.5) * Size, 0.0, Size - 1.0)) / int(VSM_PAGE_SIZE) + Lv.PageOrigin;

    uint Mip = 0u;
    while (Mip + 1u < VSM_MASK_MIPS && any((PageHi >> int(Mip)) - (PageLo >> int(Mip)) >= int(kPageRectScanSide)))
    {
        ++Mip;
    }

    const int  Side   = int(VSM_LEVEL_PAGES >> Mip);
    const uint Base   = View.ShadowLevel * VSM_MASK_CELLS_PER_LEVEL + VirtualShadowMaskMipOffset(Mip);
    const uint Bit    = View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC) ? VSM_MASK_STATIC : VSM_MASK_REQUESTED;
    const int2 CellLo = PageLo >> int(Mip);
    const int2 CellHi = PageHi >> int(Mip);
    float2 DrawnLo = float2(1.0e30, 1.0e30);
    float2 DrawnHi = float2(-1.0e30, -1.0e30);
    [ForceUnroll]
    for (uint i = 0; i < kPageRectScanSide * kPageRectScanSide / MESHLET_MESH_GROUP_SIZE; ++i)
    {
        const uint Index = ThreadID + i * MESHLET_MESH_GROUP_SIZE;
        const int2 Cell  = CellLo + int2(Index % kPageRectScanSide, Index / kPageRectScanSide);
        const uint2 Wrapped = uint2(Cell & int2(Side - 1, Side - 1));
        if (all(Cell <= CellHi) && (Masks.Get(Base + Wrapped.y * uint(Side) + Wrapped.x) & Bit) != 0u)
        {
            DrawnLo = min(DrawnLo, float2(Cell));
            DrawnHi = max(DrawnHi, float2(Cell));
        }
    }
    DrawnLo = WorkgroupMin(DrawnLo);
    DrawnHi = WorkgroupMax(DrawnHi);
    if (DrawnLo.x > DrawnHi.x)
    {
        return Empty;
    }

    const int2 RectLo = max(int2(DrawnLo) << int(Mip), PageLo) - Lv.PageOrigin;
    const int2 RectHi = min(((int2(DrawnHi) + 1) << int(Mip)) - 1, PageHi) - Lv.PageOrigin + 1;
    const float PageNdc = 2.0 * float(VSM_PAGE_SIZE) / Size;
    return float4(float2(RectLo) * PageNdc - 1.0, float2(RectHi) * PageNdc - 1.0);
}

float4 PageClipDistances(float4 Rect, float4 ClipPos)
{
    return float4(ClipPos.xy - Rect.xy * ClipPos.w, Rect.zw * ClipPos.w - ClipPos.xy);
}
#endif

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESHLET_MESH_GROUP_SIZE, 1, 1)]
void MeshMain(
    uint3 GroupID   : SV_GroupID,
    uint  ThreadID  : SV_GroupIndex,
    uint  DrawIndex : SV_DrawIndex,
    out vertices FMeshVertexOutput  Verts[MESHLET_MAX_VERTICES],
    out indices    uint3             Tris[MESHLET_MAX_TRIANGLES],
    out primitives FMeshPrimitiveOut Prims[MESHLET_MAX_TRIANGLES])
{
    uint MeshletSlot;
    FMeshletRef Ref = ResolveMeshletForWorkgroup(DrawIndex, GroupID.x, MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // Set first so output allocation overlaps the vertex phase rather than stalling it.
    SetMeshOutputCounts(VertCount, TriCount);

#ifndef MESHLET_MESH_BASE
    const uint ShadowViewport = IsMultiViewShadowDraw() ? DrawIndex : 0u;
#endif

    // Kept only for ShuffleMeshletClip, which reads it from the owning lane's registers.
    float4 Clip[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        Clip[v] = float4(0.0, 0.0, 0.0, 0.0);
    }

    // Workgroup-uniform, and needed because an unresolved meshlet leaves Ref.Inst a null base.
    if (VertCount > 0u)
    {
        // Issued before the vertex fetch so the loads overlap rather than serialize.
        const FMeshVertexContext Ctx = LoadMeshVertexContext(Ref, DrawIndex);

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // The affine export index and clamped load index must differ, see MeshletVisBuffer.slang.
            const uint StoreSlot = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
            const uint LoadSlot  = min(StoreSlot, VertCount - 1u);

            const FMeshVertexOutput VOut = MeshPassVertex(Ref, LoadSlot, Ctx);
            Clip[v] = VOut.Position;

            if (StoreSlot < VertCount)
            {
                Verts[StoreSlot] = VOut;
            }
        }
    }

    // Clip positions are in this pass's space, so the reject matches the rastered volume and winding.
#ifdef MESHLET_MESH_BASE
    const float2 ViewportSize = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);
    const bool   bNoDrawnPage = false;
#else
    const float2 ViewportSize = ShadowRasterViewForDraw(DrawIndex).TileSize;

    // A level rasters at its full virtual size, so a close meshlet would otherwise shade its whole footprint for the few pages it feeds.
    const float4 PageRect     = VirtualShadowMeshletPageRect(MeshletPass().CullViewIndex + ShadowViewport, Clip, VertCount, ThreadID);
    const bool   bNoDrawnPage = PageRect.x > PageRect.z;
    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        const uint StoreSlot = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
        if (StoreSlot < VertCount)
        {
            const float4 Distances = PageClipDistances(PageRect, Clip[v]);
            [ForceUnroll]
            for (uint Plane = 0u; Plane < 4u; ++Plane)
            {
                Verts[StoreSlot].PageClip[Plane] = Distances[Plane];
            }
        }
    }
#endif

    // Dead triangles carry SV_CullPrimitive, so there is no compaction. The clamp below needs a non-zero TriCount.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // The clamped index keeps every lane active for the shuffle, the affine index is for the export.
            const uint  StoreT = ThreadID + i * MESHLET_MESH_GROUP_SIZE;
            const uint  t      = min(StoreT, TriCount - 1u);
            const uint3 Idx    = UnpackMeshletTriangleSafe(Ref, t);

            const bool bCull = bNoDrawnPage
                             | IsDegenerateTriangleIndices(Idx)
                             | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                       ShuffleMeshletClip(Clip, Idx.y),
                                                       ShuffleMeshletClip(Clip, Idx.z), ViewportSize);

            if (StoreT < TriCount)
            {
                Tris[StoreT]                = Idx;
                Prims[StoreT].CullPrimitive = bCull;
#ifndef MESHLET_MESH_BASE
                Prims[StoreT].ViewportIndex = ShadowViewport;
#endif
            }
        }
    }
}

// The fallback without mesh shading, one instance per meshlet and one view per draw, so it writes no viewport index.
[shader("vertex")]
FMeshVertexOutput VertexMain(uint VertexIndex   : SV_VulkanVertexID,
                             uint InstanceIndex : SV_VulkanInstanceID,
                             uint DrawIndex     : SV_DrawIndex)
{
    // An invalid corner stays at the origin, so its triangle has no area.
    const FMeshletCorner Corner = ResolveMeshletCorner(DrawIndex, InstanceIndex, VertexIndex);
    if (!Corner.bValid)
    {
        return (FMeshVertexOutput)0;
    }
    return MeshPassVertex(Corner.Ref, Corner.LocalVertex, LoadMeshVertexContext(Corner.Ref, DrawIndex));
}
