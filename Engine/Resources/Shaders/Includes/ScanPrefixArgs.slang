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
