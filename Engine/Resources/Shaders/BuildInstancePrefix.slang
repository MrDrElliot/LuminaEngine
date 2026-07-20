#include "Includes/SceneGlobals.slang"

// GPU-built exclusive prefix sum over per-instance surface meshlet counts (InstanceMeshletPrefix):
// the meshlet cull binary-searches it to map a flat thread index back to (instance, local meshlet).
// Replaces the serial CPU prefix in MergeMeshDrawData and its per-frame N*4B upload.
//
// Single workgroup, grid-stride chunks: each 256-element chunk is scanned in groupshared
// (Hillis-Steele, ping-pong buffers) and offset by the running total of the prior chunks. One
// workgroup serializes the chunks, but at 256 lanes per step this stays tens of microseconds for
// even 100k+ instances; upgrade to a hierarchical scan only if the profiler ever names it.

struct FBuildInstancePrefixArgs
{
    uint  NumInstances;
    uint  _Pad0;
    uint* OutPrefix;   // NumInstances + 1 entries: [i] = exclusive sum, [N] = total
};
FBuildInstancePrefixArgs* Pass() { return GetPassArgs<FBuildInstancePrefixArgs>(); }

#define SCAN_BLOCK 256

groupshared uint gScanA[SCAN_BLOCK];
groupshared uint gScanB[SCAN_BLOCK];

[shader("compute")]
[numthreads(SCAN_BLOCK, 1, 1)]
void ComputeMain(uint ThreadID : SV_GroupIndex)
{
    const uint N = Pass().NumInstances;
    uint Running = 0u;   // uniform: every thread accumulates the same chunk totals

    for (uint Base = 0u; Base < N; Base += SCAN_BLOCK)
    {
        const uint Idx   = Base + ThreadID;
        const uint Value = (Idx < N) ? Instances()[Idx].SurfaceMeshletCount : 0u;

        gScanA[ThreadID] = Value;
        GroupMemoryBarrierWithGroupSync();

        // Hillis-Steele inclusive scan; 8 doubling steps for 256 lanes, so the
        // even step count lands the final result back in gScanA.
        bool bReadA = true;
        for (uint Offset = 1u; Offset < SCAN_BLOCK; Offset <<= 1u)
        {
            if (bReadA)
            {
                gScanB[ThreadID] = gScanA[ThreadID] + ((ThreadID >= Offset) ? gScanA[ThreadID - Offset] : 0u);
            }
            else
            {
                gScanA[ThreadID] = gScanB[ThreadID] + ((ThreadID >= Offset) ? gScanB[ThreadID - Offset] : 0u);
            }
            bReadA = !bReadA;
            GroupMemoryBarrierWithGroupSync();
        }

        if (Idx < N)
        {
            Pass().OutPrefix[Idx] = Running + gScanA[ThreadID] - Value;   // inclusive -> exclusive
        }
        Running += gScanA[SCAN_BLOCK - 1u];   // chunk total

        // All lanes read gScanA above before the next chunk overwrites it.
        GroupMemoryBarrierWithGroupSync();
    }

    if (ThreadID == 0u)
    {
        Pass().OutPrefix[N] = Running;
    }
}
