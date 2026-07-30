#include "Includes/SceneGlobals.slang"

// One-thread compute that sizes the LATE cull's indirect dispatch from the deferred-meshlet count. The early
// cull atomically appends HZB-occluded camera meshlets to the defer list and bumps DeferCount; the late cull
// re-tests ONLY that set. Without this, CullPassLate dispatched a worst-case full-scene grid where almost
// every thread early-outs. This is fully GPU-side (no CPU readback): the args are consumed by CmdDispatchIndirect.

struct FBuildCullDispatchArgs
{
    uint  DeferListCapacity;   // entries the defer list can hold; the counter can exceed it
    uint  _Pad0;
    uint* DeferCount;          // in:  deferred meshlets the early cull WANTED to append (unbounded)
    uint* DispatchArgs;        // out: {GroupCountX, GroupCountY, GroupCountZ}
    uint* OutTotals;           // out: [6] = defer demand, [7] = demand exceeded the allocation
};
FBuildCullDispatchArgs* Pass() { return GetPassArgs<FBuildCullDispatchArgs>(); }

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    // Demand, which may exceed what fit -- the early cull keeps counting so this can size the next
    // allocation, the same feedback pattern the draw list and the instance buffer use.
    const uint Requested = Pass().DeferCount[0];
    const uint Count     = min(Requested, Pass().DeferListCapacity);

    Pass().DispatchArgs[0] = (Count + 63u) / 64u;   // LOCAL_SIZE_X = 64 in CullMeshlets.slang; 0 when none deferred
    Pass().DispatchArgs[1] = 1u;
    Pass().DispatchArgs[2] = 1u;

    Pass().OutTotals[6] = Requested;
    Pass().OutTotals[7] = (Requested > Pass().DeferListCapacity) ? 1u : 0u;
}
