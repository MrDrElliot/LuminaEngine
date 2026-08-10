#include "Includes/SceneGlobals.slang"

// Lays out the meshlet cull's flat dispatch, then publishes what it appended as the draw slices the
// mesh stage reads. Same serial one-workgroup scan as BuildDrawPrefix: NumViews * NumDraws is in the
// hundreds, so a hierarchical scan would cost more in launch overhead than it saves.
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
    uint  _Pad0;
    uint  _Pad1;

    FRenderBucket*                   Buckets;
    // One workgroup per written block, Y-folded exactly as BuildDrawPrefix folds the block dispatch.
    FDispatchIndirectArguments*      OutCullDispatchArgs;
    // [(bucket * MESHLET_SLICE_COUNT + slice) * SubDrawsPerSlice + subdraw]
    FDrawMeshTasksIndirectArguments* OutMeshDrawArgs;
};
FBuildMeshletCullArgsArgs* Pass() { return GetPassArgs<FBuildMeshletCullArgsArgs>(); }

// The PRE pass is a running prefix, so it is serial by nature and does 3 writes per bucket. The POST
// pass has no cross-bucket dependency and writes NumBuckets * SLICE_COUNT * SubDrawsPerSlice arguments,
// which is thousands -- serial, that was the most expensive thing in the cull. It runs one thread per
// bucket instead; the dispatch supplies one group for PRE and ceil(NumBuckets / 64) for POST.
#define BUILD_CULL_ARGS_GROUP 64

[shader("compute")]
[numthreads(BUILD_CULL_ARGS_GROUP, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumBuckets = Pass().NumViews * Pass().NumDraws;
    const uint Slice      = Pass().Slice;

    if (Pass().bPost == 0u)
    {
        // One thread owns the whole scan: the prefix is inherently serial and only NumBuckets long.
        if (ThreadID.x != 0u)
        {
            return;
        }

        uint Running = 0u;
        for (uint i = 0u; i < NumBuckets; ++i)
        {
            FRenderBucket* Bucket = Pass().Buckets + i;

            // Blocks are dense inside a bucket but sparse across the arena, so the flat dispatch needs
            // its own prefix. Clamped: the append cursor counts past capacity on an overflow frame.
            Bucket.CullWorkBase = Running;
            Running += min(Bucket.BlockCursor, Bucket.BlockCapacity);

            // Where this phase starts appending, relative to DrawBase like the cursor itself.
            Bucket.SliceBase[Slice]  = min(Bucket.DrawCursor, Bucket.DrawCapacity);
            Bucket.SliceCount[Slice] = 0u;
        }

        FDispatchIndirectArguments Args;
        Args.GroupX = min(Running, (uint)MAX_DISPATCH_AXIS);
        Args.GroupY = (Running + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
        Args.GroupZ = 1u;
        Pass().OutCullDispatchArgs[0] = Args;
        return;
    }

    {
        const uint i = ThreadID.x;
        if (i >= NumBuckets)
        {
            return;
        }

        FRenderBucket* Bucket = Pass().Buckets + i;

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
            Bucket.SubDrawCount[s] = (Count + Pass().MaxMeshGroups - 1u) / Pass().MaxMeshGroups;

            for (uint sd = 0u; sd < Pass().SubDrawsPerSlice; ++sd)
            {
                const uint First = sd * Pass().MaxMeshGroups;

                FDrawMeshTasksIndirectArguments DrawArgs;
                DrawArgs.GroupCountX = (First < Count) ? min(Count - First, Pass().MaxMeshGroups) : 0u;
                DrawArgs.GroupCountY = 1u;
                DrawArgs.GroupCountZ = 1u;

                const uint Index = (i * (uint)MESHLET_SLICE_COUNT + s) * Pass().SubDrawsPerSlice + sd;
                Pass().OutMeshDrawArgs[Index] = DrawArgs;
            }
        }
    }
}
