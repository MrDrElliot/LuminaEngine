#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"

// One thread per (visible instance, view), appending that pair's blocks into the region
// CullInstances reserved for it. The LOD pick and the reject set below must match CullInstances
// exactly -- see the reject block in ComputeMain.

struct FBuildMeshletBlocksArgs
{
    uint            NumViews;
    uint            NumBatches;
    // Clamped exactly as BuildDrawPrefix clamps it: the append cursor counts past capacity on overflow.
    uint            MaxVisibleInstances;
    uint            NumSurfaceDescs;

    uint*           InstanceCount;
    FSurfaceDesc*   SurfaceDescs;
    FRenderBucket*  Buckets;
    FMeshletBlock*  OutBlockList;
};
FBuildMeshletBlocksArgs* Pass() { return GetPassArgs<FBuildMeshletBlocksArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumViews         = max(Pass().NumViews, 1u);
    const uint VisibleInstances = min(Pass().InstanceCount[0], Pass().MaxVisibleInstances);

    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint Flat       = ThreadID.x + ThreadID.y * MAX_DISPATCH_AXIS * 64u;
    const uint InstanceID = Flat / NumViews;
    const uint ViewIndex  = Flat % NumViews;

    if (InstanceID >= VisibleInstances || ViewIndex >= Pass().NumViews)
    {
        return;
    }

    FGPUInstance* Inst = Instances() + InstanceID;
    if (Inst.MeshletHeader == nullptr)
    {
        return;
    }

    const FCullView View = CullViews()[ViewIndex];

    // The same call CullInstances sized this bucket with; see IsInstanceVisibleToView.
    if (!IsInstanceVisibleToView(View, Inst.BoundsCenter, Inst.BoundsRadius,
                                 Inst.HasFlag(EInstanceFlags::NoCull),
                                 Inst.HasFlag(EInstanceFlags::CastShadow)))
    {
        return;
    }

    // Byte-for-byte the pick CullInstances reserved this region from.
    const bool bPerViewLOD = Inst.SurfaceDescIndex != kNoSurfaceDesc
                          && Inst.SurfaceDescIndex < Pass().NumSurfaceDescs;

    FSurfaceDesc Desc = (FSurfaceDesc)0;
    if (bPerViewLOD)
    {
        Desc = Pass().SurfaceDescs[Inst.SurfaceDescIndex];
    }

    const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

    uint2 Range;
    if (bShadowView && bPerViewLOD && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
    {
        Range = SelectCascadeShadowMeshletRange(Desc, GetCameraPosition(), Inst.SphereBounds,
                                                View.CascadeIndex,
                                                Scene().CullData.ShadowLODBias,
                                                Scene().CullData.ShadowCoarseLODDistSq);
    }
    else if (bShadowView)
    {
        Range = uint2(Inst.ShadowMeshletOffset, Inst.ShadowMeshletCount);
    }
    else if (bPerViewLOD)
    {
        Range = SelectViewMeshletRange(Desc, View.ViewOrigin, Inst.SphereBounds);
    }
    else
    {
        Range = uint2(Inst.SurfaceMeshletOffset, Inst.SurfaceMeshletCount);
    }

    // Unbounded, this walks the header arrays past the allocation: scattered BDA reads return garbage
    // rather than faulting, so it surfaces as a multi-second hang and a TDR with no page fault.
    const uint MeshletTotal = Inst.MeshletTotalCount;
    if (Range.y == 0u || Range.x >= MeshletTotal)
    {
        return;
    }
    const uint Count = min(Range.y, MeshletTotal - Range.x);

    // Past this the pair owns no region and ArgIdx would address the next view's.
    if (Inst.DrawID >= Pass().NumBatches)
    {
        return;
    }

    FRenderBucket* Bucket = Pass().Buckets + (ViewIndex * Pass().NumBatches + Inst.DrawID);
    const uint Blocks = (Count + (MESHLET_TASK_GROUP_SIZE - 1u)) / MESHLET_TASK_GROUP_SIZE;

    // One atomic for the whole run: an instance's blocks are contiguous by construction.
    uint Granted;
    const uint Slot = AppendReserveClamped(&Bucket.BlockCursor,
                                           Bucket.BlockCapacity,
                                           Bucket.BlockBase,
                                           Blocks, Granted);

    for (uint b = 0u; b < Granted; ++b)
    {
        const uint First = b * MESHLET_TASK_GROUP_SIZE;

        FMeshletBlock Block;
        Block.InstanceID         = InstanceID;
        Block.PackedBaseAndCount = FMeshletBlock::Pack(Range.x + First,
                                                       min(MESHLET_TASK_GROUP_SIZE, Count - First));
        Pass().OutBlockList[Slot + b] = Block;
    }
}
