#pragma once
#include "SceneGlobals.slang"

// ONE declaration of the instance-prefix scan's push block, shared by all three passes
// (ScanPrefixBlocks / ScanPrefixSums / ScanPrefixAdd), because the C++ side dispatches all three with a
// single FScanPC.
//
// This is deliberately not three per-shader structs. It was, and pass 2 declared BlockSums one slot early
// because it had no use for the Prefix pointer -- so it scanned the prefix buffer in place and left the
// block sums unscanned. Nothing caught it: the C++ static_assert only validates the CPU-side layout, and
// there is no cross-check between a shader's view of a push block and the struct the CPU pushes. The
// corruption surfaced as a different random subset of meshes rendering each frame.
//
// If a pass does not need one of these pointers, it still gets it. Do not specialize this per pass.

#define SCAN_BLOCK 256

struct FScanPrefixArgs
{
    uint  _Pad0;
    uint  _Pad1;
    uint* CountPtr;    // in:  [0] = surviving instance count (Totals[1])
    uint* Prefix;      // in/out: Count + 1 entries; [Count] = total
    uint* BlockSums;   // in/out: ceil(Count / SCAN_BLOCK) + 1 entries; [NumBlocks] = grand total
};
FScanPrefixArgs* Pass() { return GetPassArgs<FScanPrefixArgs>(); }

// The element count all three passes scan.
//
// Clamped to the visible-instance CAPACITY the CPU published, not taken raw from the totals block.
// CountPtr[0] is GPU-produced, so nothing on the CPU has seen it; if it ever exceeds the allocation
// -- a totals block read before its producer landed, a resize that did not take, a capacity two
// consumers disagree about -- then Instances()[Idx] resolves to an unmapped address and the fault
// takes the device with it, rather than producing a wrong pixel. MaterialDepth and DeferredMaterial
// already bound their instance index against this same value for the same reason; the scan is what
// was missing it.
//
// It lives here, next to the shared push block, for the reason written above it: all three passes
// have to derive N identically, and a per-shader clamp is exactly the kind of divergence that made
// pass 2 fold in block sums pass 1 never wrote.
uint ScanCount()
{
    // No instance buffer means no elements, not "scan whatever the totals block says". The root only
    // carries this address when the visible-instance ring is allocated, so null is a state the CPU
    // can legitimately publish -- and every element the scan then touched would be an offset off zero.
    if (Instances() == nullptr)
    {
        return 0u;
    }

    return min(Pass().CountPtr[0], Scene().CullData.InstanceNum);
}
