#include "Includes/ScanPrefixArgs.slang"

// Pass 1 of 3 of the per-instance meshlet prefix scan (replaces the single-workgroup scan in
// BuildInstancePrefix.slang, which measured 1.94 ms of pure GPU serialization at ~500k instances:
// one workgroup walking ~2000 sequential 256-element chunks on a single SM).
//
// Standard three-kernel scan. This pass runs one workgroup PER BLOCK, all blocks concurrent across every
// SM, and produces two things:
//   OutPrefix[i]      block-LOCAL exclusive prefix (pass 3 adds the block's global offset back)
//   OutBlockSums[b]   that block's total, which pass 2 scans
//
// Domain comes from an indirect dispatch off the GPU-side survivor count, so no CPU readback and no
// dispatching over the worst case.



groupshared uint gScanA[SCAN_BLOCK];
groupshared uint gScanB[SCAN_BLOCK];

[shader("compute")]
[numthreads(SCAN_BLOCK, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupIndex)
{
    const uint N     = Pass().CountPtr[0];
    const uint Block = GroupID.x + GroupID.y * 65535u;   // same Y fold the cull dispatches use
    const uint Idx   = Block * SCAN_BLOCK + Lane;

    // max, not the surface count: a shadow-only view walks the SHADOW LOD, which is capped at
    // MESHLET_MAX_SHADOW_LOD and so can be FINER (more meshlets) than the camera LOD on distant casters.
    // This is the per-instance work domain and must match CullMeshlets' walk, CullInstances'
    // OutBatchMeshletCount and the CPU's skinned seed exactly.
    const uint Value = (Idx < N)
        ? max(Instances()[Idx].SurfaceMeshletCount, Instances()[Idx].ShadowMeshletCount)
        : 0u;

    gScanA[Lane] = Value;
    GroupMemoryBarrierWithGroupSync();

    // Hillis-Steele inclusive scan; 8 doubling steps for 256 lanes, so the even step count lands the
    // final result back in gScanA.
    bool bReadA = true;
    for (uint Offset = 1u; Offset < SCAN_BLOCK; Offset <<= 1u)
    {
        if (bReadA)
        {
            gScanB[Lane] = gScanA[Lane] + ((Lane >= Offset) ? gScanA[Lane - Offset] : 0u);
        }
        else
        {
            gScanA[Lane] = gScanB[Lane] + ((Lane >= Offset) ? gScanB[Lane - Offset] : 0u);
        }
        bReadA = !bReadA;
        GroupMemoryBarrierWithGroupSync();
    }

    if (Idx < N)
    {
        Pass().Prefix[Idx] = gScanA[Lane] - Value;   // inclusive -> exclusive, block-local
    }

    // Only blocks that actually cover live elements have a meaningful sum; the rest still publish 0 so
    // pass 2 can scan a dense array without knowing which blocks were partial.
    if (Lane == SCAN_BLOCK - 1u)
    {
        Pass().BlockSums[Block] = gScanA[SCAN_BLOCK - 1u];
    }
}
