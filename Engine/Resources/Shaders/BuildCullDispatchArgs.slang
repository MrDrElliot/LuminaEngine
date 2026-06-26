#include "Includes/SceneGlobals.slang"

// One-thread compute that sizes the LATE cull's indirect dispatch from the deferred-meshlet count. The early
// cull atomically appends HZB-occluded camera meshlets to the defer list and bumps DeferCount; the late cull
// re-tests ONLY that set. Without this, CullPassLate dispatched a worst-case full-scene grid where almost
// every thread early-outs. This is fully GPU-side (no CPU readback): the args are consumed by CmdDispatchIndirect.

struct FBuildCullDispatchArgs
{
    uint* DeferCount;    // in:  number of deferred meshlets (early cull's atomic counter)
    uint* DispatchArgs;  // out: {GroupCountX, GroupCountY, GroupCountZ}
};
FBuildCullDispatchArgs* Pass() { return GetPassArgs<FBuildCullDispatchArgs>(); }

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    const uint Count = Pass().DeferCount[0];
    Pass().DispatchArgs[0] = (Count + 63u) / 64u;   // LOCAL_SIZE_X = 64 in CullMeshlets.slang; 0 when none deferred
    Pass().DispatchArgs[1] = 1u;
    Pass().DispatchArgs[2] = 1u;
}
