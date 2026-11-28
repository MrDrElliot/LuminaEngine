#pragma once
#include "Includes/TerrainData.slang"
#include "Includes/MeshletGeometry.slang"

// Terrain rasters into the VisBuffer like any mesh, one workgroup per visible quadtree node of one terrain per draw.

struct FTerrainDrawPush
{
    uint TerrainIndex;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};
FTerrainDrawPush* DrawArgs() { return GetPassArgs<FTerrainDrawPush>(); }

FMaterialVertexInputs EvalTerrainVertex(FTerrainRenderParams TerrainParams, float3 WorldPos, float3 NormalWS, float2 HeightUV)
{
    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    return Material;
}

float4 TerrainClipPosition(FTerrainRenderParams Params, FTerrainVisibleNode Vis, uint GridIndex, inout FTerrainPage Cache)
{
    const FTerrainVertex V = TerrainNodeVertex(Params, Vis, GridIndex, Cache);
    GTerrainShadingMip = float(TerrainNodeLevel(Params, UnpackTerrainNode(Vis.Node).Depth));
    const FMaterialVertexInputs Material = EvalTerrainVertex(Params, V.WorldPos, V.NormalWS, V.HeightUV);
    return CameraClipPosition(CameraViewPosition(V.WorldPos + Material.WorldPositionOffset));
}

struct FTerrainVisVertex
{
    float4 Position : SV_Position;
};

struct FTerrainVisPrimitive
{
    uint VisID : SV_PrimitiveID;
};

#define TERRAIN_VERT_ITERS ((TERRAIN_MESHLET_MAX_VERTICES  + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)
#define TERRAIN_TRI_ITERS  ((TERRAIN_MESHLET_MAX_TRIANGLES + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESHLET_MESH_GROUP_SIZE, 1, 1)]
void MeshMain(
    uint3 GroupID  : SV_GroupID,
    uint  ThreadID : SV_GroupIndex,
    out vertices   FTerrainVisVertex    Verts[TERRAIN_MESHLET_MAX_VERTICES],
    out indices    uint3                Tris[TERRAIN_MESHLET_MAX_TRIANGLES],
    out primitives FTerrainVisPrimitive Prims[TERRAIN_MESHLET_MAX_TRIANGLES])
{
    GTerrainIndex = DrawArgs().TerrainIndex;
    FTerrainRecord* Record = TerrainArgs();

    // Workgroup-uniform, so the output count below is too.
    const bool bValid = GroupID.x < Record.Visible.Count;
    SetMeshOutputCounts(bValid ? TERRAIN_MESHLET_MAX_VERTICES : 0u, bValid ? TERRAIN_MESHLET_MAX_TRIANGLES : 0u);
    if (!bValid)
    {
        return;
    }

    const FTerrainRenderParams Params = Record.Params;
    const FTerrainVisibleNode  Vis    = Record.Visible.Get(GroupID.x);
    const uint                 Slot   = Record.VisibleBase + GroupID.x;

    FTerrainPage Cache = (FTerrainPage)0;
    [ForceUnroll]
    for (uint v = 0; v < TERRAIN_VERT_ITERS; ++v)
    {
        const uint Index = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
        if (Index < TERRAIN_MESHLET_MAX_VERTICES)
        {
            Verts[Index].Position = TerrainClipPosition(Params, Vis, Index, Cache);
        }
    }

    [ForceUnroll]
    for (uint t = 0; t < TERRAIN_TRI_ITERS; ++t)
    {
        const uint Tri = ThreadID + t * MESHLET_MESH_GROUP_SIZE;
        if (Tri < TERRAIN_MESHLET_MAX_TRIANGLES)
        {
            Tris[Tri]        = TerrainTriangleCorners(Tri);
            Prims[Tri].VisID = PackTerrainVisID(Slot, Tri);
        }
    }
}

struct FTerrainVisVertexStageOutput
{
    float4               Position : SV_Position;
    // The slot's VisID at triangle 0, which TerrainVisBufferPixel completes with the primitive ID.
    nointerpolation uint VisBase  : TERRAIN_VIS_BASE;
};

// The fallback without mesh shading, drawn indexed over one node's grid with one instance per visible node.
[shader("vertex")]
FTerrainVisVertexStageOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    GTerrainIndex = DrawArgs().TerrainIndex;
    FTerrainRecord* Record = TerrainArgs();

    FTerrainVisVertexStageOutput Out = (FTerrainVisVertexStageOutput)0;
    if (InstanceIndex >= Record.Visible.Count)
    {
        return Out;
    }

    const FTerrainVisibleNode Vis = Record.Visible.Get(InstanceIndex);

    FTerrainPage Cache = (FTerrainPage)0;
    Out.Position = TerrainClipPosition(Record.Params, Vis, VertexIndex, Cache);
    Out.VisBase  = PackTerrainVisID(Record.VisibleBase + InstanceIndex, 0u);
    return Out;
}
