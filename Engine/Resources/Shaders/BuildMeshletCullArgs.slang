#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// Runs before the cull to lay out the flat block dispatch, and after it to publish the phase's draw slices.

// One meshlet sub-draw, read as mesh-task group counts or as an unindexed instanced draw, and mirrored by FMeshletDrawArgs.
struct FMeshletDrawArgs
{
    uint Words[4];
};

// The indirect dispatch reads the first three words, and MeshletCull.slang reads how many blocks each group takes.
struct FMeshletCullDispatch
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
    uint BlocksPerGroup;
};

struct FBuildMeshletCullArgsArgs
{
    uint  NumViews;
    uint  NumDraws;
    // MESHLET_SLICE_EARLY or MESHLET_SLICE_LATE, never MESHLET_SLICE_ALL since that slice is derived.
    uint  Slice;
    // 0 = before the cull dispatch, 1 = after it.
    uint  bPost;
    // maxMeshWorkGroupCount[0], and a slice needing more is split across sub-draws rather than truncated.
    uint  MaxMeshGroups;
    // Sub-draw slots reserved per (bucket, slice).
    uint  SubDrawsPerSlice;
    // 0 keeps every block in front for both phases, as the block build does when splitting is off.
    uint  bSplitPhases;
    // 1 keeps near cascades out of this phase, 2 lays out only them for the dispatch that culls against the receiver mask.
    uint  bDeferCascades;
    // Corners per meshlet on the vertex path, or 0 for mesh shading.
    uint  VertexPathCorners;
    uint  _Pad0;
    uint  _Pad1;
    uint  _Pad2;

    GPUSpan<FRenderBucket>                   Buckets;
    // One workgroup per written block, Y-folded exactly as BuildDrawPrefix folds the block dispatch.
    GPUSpan<FMeshletCullDispatch>            OutCullDispatchArgs;
    // [(bucket * MESHLET_SLICE_COUNT + slice) * SubDrawsPerSlice + subdraw]
    GPUSpan<FMeshletDrawArgs>                OutMeshDrawArgs;
};
FBuildMeshletCullArgsArgs* Pass() { return GetPassArgs<FBuildMeshletCullArgsArgs>(); }

// The blocks a phase culls. A split bucket holds the early blocks in front and the late ones at the back of its region.
uint PhaseBlocks(FRenderBucket* Bucket, uint ViewIndex, bool bLate, bool bSplitPhases)
{
    const uint Front = min(Bucket.BlockCursor, Bucket.BlockCapacity);
    const bool bDeferred = Pass().bDeferCascades != 0u && CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_CASCADE)
                        && !CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_FAR_CASCADE);
    if (Pass().bDeferCascades == 2u || bDeferred)
    {
        return (Pass().bDeferCascades == 2u && bDeferred) ? Front : 0u;
    }
    if (!bLate)
    {
        return Front;
    }
    if (!CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_MESHLET_HIZ))
    {
        return 0u;
    }
    return bSplitPhases && BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity)
         ? min(Bucket.BlockBackCount, Bucket.BlockCapacity - Front)
         : Front;
}

// The pre pass is one workgroup scanning bucket runs, the post pass one thread per bucket.
#define BUILD_CULL_ARGS_GROUP 64

[shader("compute")]
[numthreads(BUILD_CULL_ARGS_GROUP, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumBuckets = Pass().NumViews * Pass().NumDraws;
    const uint Slice      = Pass().Slice;

    if (Pass().bPost == 0u)
    {
        const uint PerThread = (NumBuckets + BUILD_CULL_ARGS_GROUP - 1u) / BUILD_CULL_ARGS_GROUP;
        const uint First     = min(ThreadID.x * PerThread, NumBuckets);
        const uint Last      = min(First + PerThread, NumBuckets);
        const uint NumDraws  = max(Pass().NumDraws, 1u);

        // The late phase only culls two-phase views, so every other bucket launches no workgroup at all.
        const bool bLate = (Slice == MESHLET_SLICE_LATE);

        uint Local = 0u;
        uint LocalMeshlets = 0u;
        uint LocalBlocks = 0u;
        for (uint i = First; i < Last; ++i)
        {
            // Blocks are dense inside a bucket but sparse across the arena, so the flat dispatch needs its own prefix.
            FRenderBucket* Bucket = Pass().Buckets.At(i);
            const uint Blocks = PhaseBlocks(Bucket, i / NumDraws, bLate, Pass().bSplitPhases != 0u);
            Local += Blocks;
            if (Blocks > 0u)
            {
                LocalMeshlets += Bucket.DrawDemand;
                LocalBlocks   += Bucket.BlockDemand;
            }
        }

        uint2 Total;
        uint Running = GroupExclusiveScan(uint2(Local, LocalMeshlets), ThreadID.x, BUILD_CULL_ARGS_GROUP, Total).x;
        GroupMemoryBarrierWithGroupSync();
        uint2 BlockTotal;
        GroupExclusiveScan(uint2(LocalBlocks, 0u), ThreadID.x, BUILD_CULL_ARGS_GROUP, BlockTotal);

        for (uint i = First; i < Last; ++i)
        {
            FRenderBucket* Bucket = Pass().Buckets.At(i);

            Bucket.CullWorkBase = Running;
            Running += PhaseBlocks(Bucket, i / NumDraws, bLate, Pass().bSplitPhases != 0u);

            // Where this phase starts appending, relative to DrawBase like the cursor itself.
            Bucket.SliceBase[Slice]  = min(Bucket.DrawCursor, Bucket.DrawCapacity);
            Bucket.SliceCount[Slice] = 0u;
        }

        if (ThreadID.x == 0u)
        {
            // Small meshes fill a block with a meshlet or two, so a group takes as many blocks as fill its lanes on average.
            const uint MeshletsPerBlock = clamp((Total.y + max(BlockTotal.x, 1u) - 1u) / max(BlockTotal.x, 1u), 1u, (uint)MESHLET_CULL_GROUP_SIZE);
            const uint BlocksPerGroup   = (uint)MESHLET_CULL_GROUP_SIZE / MeshletsPerBlock;
            const uint Groups           = (Total.x + BlocksPerGroup - 1u) / BlocksPerGroup;

            FMeshletCullDispatch Args;
            Args.GroupX = min(Groups, (uint)MAX_DISPATCH_AXIS);
            Args.GroupY = (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
            Args.GroupZ = 1u;
            Args.BlocksPerGroup = BlocksPerGroup;
            Pass().OutCullDispatchArgs.Set(0u, Args);
        }
        return;
    }

    {
        const uint i = ThreadID.x;
        if (i >= NumBuckets)
        {
            return;
        }

        FRenderBucket* Bucket = Pass().Buckets.At(i);

        const uint Written = min(Bucket.DrawCursor, Bucket.DrawCapacity);
        const uint Start   = Bucket.SliceBase[Slice];
        Bucket.SliceCount[Slice] = (Written > Start) ? (Written - Start) : 0u;

        // Single-phase passes draw the whole region and run after both phases, so it is derived here.
        Bucket.SliceBase[MESHLET_SLICE_ALL]  = 0u;
        Bucket.SliceCount[MESHLET_SLICE_ALL] = Written;

        for (uint s = 0u; s < (uint)MESHLET_SLICE_COUNT; ++s)
        {
            const uint Count = Bucket.SliceCount[s];

            // Spelled to survive MaxMeshGroups near UINT32_MAX, where the rounded-up division wraps to zero.
            Bucket.SubDrawCount[s] = (Count == 0u) ? 0u : (((Count - 1u) / Pass().MaxMeshGroups) + 1u);

            for (uint sd = 0u; sd < Pass().SubDrawsPerSlice; ++sd)
            {
                const uint First = sd * Pass().MaxMeshGroups;

                const uint Meshlets = (First < Count) ? min(Count - First, Pass().MaxMeshGroups) : 0u;

                // The vertex path draws one instance per meshlet where the mesh path launches one workgroup.
                const bool bVertexPath = Pass().VertexPathCorners != 0u;
                FMeshletDrawArgs DrawArgs;
                DrawArgs.Words[0] = bVertexPath ? Pass().VertexPathCorners : Meshlets;
                DrawArgs.Words[1] = bVertexPath ? Meshlets : 1u;
                DrawArgs.Words[2] = bVertexPath ? 0u : 1u;
                DrawArgs.Words[3] = 0u;

                const uint Index = (i * (uint)MESHLET_SLICE_COUNT + s) * Pass().SubDrawsPerSlice + sd;
                Pass().OutMeshDrawArgs.Set(Index, DrawArgs);
            }
        }
    }
}
