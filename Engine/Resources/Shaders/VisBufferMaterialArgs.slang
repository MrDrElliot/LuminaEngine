#include "Includes/SceneGlobals.slang"
#include "Shared/SharedConstants.h"

// One workgroup per binned tile for each slot, folded across Y because a 4K frame's tile count can overrun maxComputeWorkGroupCount[0].

struct FMaterialArgsArgs
{
    GPUSpan<uint> Counts;   // tiles each slot owns; its count is the slot count
    GPUSpan<uint> Args;     // NumSlots * 3, one dispatch triple per slot
};
FMaterialArgsArgs* Pass() { return GetPassArgs<FMaterialArgsArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint Slot = DTid.x;
    if (Slot >= Pass().Counts.Count)
    {
        return;
    }

    const uint Groups = SpanGet(&Pass().Counts, Slot);
    SpanSet(&Pass().Args, Slot * 3u + 0u, min(Groups, (uint)MAX_DISPATCH_AXIS));
    SpanSet(&Pass().Args, Slot * 3u + 1u, (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS);
    SpanSet(&Pass().Args, Slot * 3u + 2u, 1u);
}
