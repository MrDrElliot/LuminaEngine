#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// Lays out the meshlet cull's flat dispatch, then publishes what it appended as the draw slices the
// mesh stage reads.
//
// Runs twice per cull phase. BEFORE the cull it assigns each bucket a dense offset in the flat block
// dispatch and records where the phase starts appending; AFTER it turns the cursor movement into that
// phase's slice and writes the per-slice indirect draw arguments.

struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FBuildMeshletCullArgsArgs
{
    uint  NumViews;
    uint  NumDraws;
    // MESHLET_SLICE_EARLY or MESHLET_SLICE_LATE. Never ALL -- that slice is derived, not appended.
    uint  Slice;
    // 0 = before the cull dispatch, 1 = after it.
    uint  bPost;
    // maxMeshWorkGroupCount[0]. One mesh workgroup per surviving meshlet, so a slice needing more than
    // this is split across consecutive sub-draws rather than truncated.
    uint  MaxMeshGroups;
    // Sub-draw slots reserved per (bucket, slice).
    uint  SubDrawsPerSlice;
    // 0 keeps every block in front for both phases, as the block build does when splitting is off.
    uint  bSplitPhases;
    uint  _Pad1;

    GPUSpan<FRenderBucket>                   Buckets;
    // One workgroup per written block, Y-folded exactly as BuildDrawPrefix folds the block dispatch.
    GPUSpan<FDispatchIndirectArguments>      OutCullDispatchArgs;
    // [(bucket * MESHLET_SLICE_COUNT + slice) * SubDrawsPerSlice + subdraw]
    GPUSpan<FDrawMeshTasksIndirectArguments> OutMeshDrawArgs;
};
FBuildMeshletCullArgsArgs* Pass() { return GetPassArgs<FBuildMeshletCullArgsArgs>(); }

// The blocks a phase culls. A split bucket holds the early blocks in front and the late ones at the back of its region.
uint PhaseBlocks(FRenderBucket* Bucket, uint ViewIndex, bool bLate, bool bSplitPhases)
{
    const uint Front = min(Bucket.BlockCursor, Bucket.BlockCapacity);
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

// PRE is one workgroup scanning contiguous runs of buckets; POST is one thread per bucket over ceil(NumBuckets / 64) groups.
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
        for (uint i = First; i < Last; ++i)
        {
            // Blocks are dense inside a bucket but sparse across the arena, so the flat dispatch needs its own prefix.
            Local += PhaseBlocks(SpanAt(&Pass().Buckets, i), i / NumDraws, bLate, Pass().bSplitPhases != 0u);
        }

        uint2 Total;
        uint Running = GroupExclusiveScan(uint2(Local, 0u), ThreadID.x, BUILD_CULL_ARGS_GROUP, Total).x;

        for (uint i = First; i < Last; ++i)
        {
            FRenderBucket* Bucket = SpanAt(&Pass().Buckets, i);

            Bucket.CullWorkBase = Running;
            Running += PhaseBlocks(Bucket, i / NumDraws, bLate, Pass().bSplitPhases != 0u);

            // Where this phase starts appending, relative to DrawBase like the cursor itself.
            Bucket.SliceBase[Slice]  = min(Bucket.DrawCursor, Bucket.DrawCapacity);
            Bucket.SliceCount[Slice] = 0u;
        }

        if (ThreadID.x == 0u)
        {
            FDispatchIndirectArguments Args;
            Args.GroupX = min(Total.x, (uint)MAX_DISPATCH_AXIS);
            Args.GroupY = (Total.x + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
            Args.GroupZ = 1u;
            SpanSet(&Pass().OutCullDispatchArgs, 0u, Args);
        }
        return;
    }

    {
        const uint i = ThreadID.x;
        if (i >= NumBuckets)
        {
            return;
        }

        FRenderBucket* Bucket = SpanAt(&Pass().Buckets, i);

        const uint Written = min(Bucket.DrawCursor, Bucket.DrawCapacity);
        const uint Start   = Bucket.SliceBase[Slice];
        Bucket.SliceCount[Slice] = (Written > Start) ? (Written - Start) : 0u;

        // ALL is the whole region, and it is what every single-phase pass draws. Derived rather than
        // appended: those passes all run after both phases, so the cursor is final by then.
        Bucket.SliceBase[MESHLET_SLICE_ALL]  = 0u;
        Bucket.SliceCount[MESHLET_SLICE_ALL] = Written;

        for (uint s = 0u; s < (uint)MESHLET_SLICE_COUNT; ++s)
        {
            const uint Count = Bucket.SliceCount[s];

            // Zero meshlets means zero sub-draws, so an empty slice costs the command processor nothing
            // rather than launching an empty grid.
            //
            // Spelled to survive MaxMeshGroups near UINT32_MAX. The obvious (Count + Max - 1) / Max WRAPS
            // there -- with Max == 0xFFFFFFFF the numerator is Count - 2 (mod 2^32), so the quotient is 0
            // for EVERY non-empty slice and the indirect draw draws nothing at all, silently and with no
            // validation error. GroupCountX below uses min() and stayed correct, which is what made the
            // capture read as "args are fine, count is zero". Devices really do report that value.
            Bucket.SubDrawCount[s] = (Count == 0u) ? 0u : (((Count - 1u) / Pass().MaxMeshGroups) + 1u);

            for (uint sd = 0u; sd < Pass().SubDrawsPerSlice; ++sd)
            {
                const uint First = sd * Pass().MaxMeshGroups;

                FDrawMeshTasksIndirectArguments DrawArgs;
                DrawArgs.GroupCountX = (First < Count) ? min(Count - First, Pass().MaxMeshGroups) : 0u;
                DrawArgs.GroupCountY = 1u;
                DrawArgs.GroupCountZ = 1u;

                const uint Index = (i * (uint)MESHLET_SLICE_COUNT + s) * Pass().SubDrawsPerSlice + sd;
                SpanSet(&Pass().OutMeshDrawArgs, Index, DrawArgs);
            }
        }
    }
}
