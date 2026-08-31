#include "Includes/AppendBuffer.slang"
#include "Includes/SceneGlobals.slang"

// One thread per (visible instance, view), appending that pair's blocks into the region CullInstances
// reserved for it. It reserved against a range it WROTE, and this reads that range back, so the two
// cannot disagree -- there is no LOD pick or reject set here to keep in step.

struct FBuildMeshletBlocksArgs
{
    uint            NumViews;
    uint            NumBatches;
    // Clamped exactly as BuildDrawPrefix clamps it: the append cursor counts past capacity on overflow.
    uint            MaxVisibleInstances;
    uint            _Pad0;

    uint*           InstanceCount;
    // (base, count) per (visible instance, view), written by CullInstances at reservation time.
    uint2*          InstanceViewRanges;
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

    // The range CullInstances reserved this pair's region from, read rather than re-derived. That is the
    // whole point: the two used to compute it independently in matching branch order, and a drift
    // between them handed one bucket room nobody used and silently dropped another's newest mesh.
    // Already clamped to the mesh's meshlet total, and zero when the pair is invisible or out of range.
    const uint2 Range = Pass().InstanceViewRanges[Flat];
    const uint  Count = Range.y;
    if (Count == 0u)
    {
        return;
    }

    FGPUInstance* Inst = Instances() + InstanceID;

    // Only for the bucket index; the reject itself already happened where the range was decided.
    const uint DrawID = FGPUInstance::GetDrawID(Inst);
    if (DrawID >= Pass().NumBatches)
    {
        return;
    }

    FRenderBucket* Bucket = Pass().Buckets + (ViewIndex * Pass().NumBatches + DrawID);
    const uint Blocks = (Count + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;

    // One atomic for the whole run: an instance's blocks are contiguous by construction.
    uint Granted;
    const uint Slot = AppendReserveClamped(&Bucket.BlockCursor,
                                           Bucket.BlockCapacity,
                                           Bucket.BlockBase,
                                           Blocks, Granted);

    for (uint b = 0u; b < Granted; ++b)
    {
        const uint First = b * MESHLET_CULL_GROUP_SIZE;

        FMeshletBlock Block;
        Block.InstanceID         = InstanceID;
        Block.PackedBaseAndCount = FMeshletBlock::Pack(Range.x + First,
                                                       min(MESHLET_CULL_GROUP_SIZE, Count - First));
        Pass().OutBlockList[Slot + b] = Block;
    }
}
