#include "Includes/ScanPrefixArgs.slang"

// Pass 2 of 3: in-place exclusive scan over the per-block sums pass 1 produced, plus the grand total.
//
// Still a single workgroup, and that is fine -- the domain is ceil(Count / 256), so the ~2000 sequential
// chunks the old one-kernel scan walked at 500k instances become ~8 here. This is the whole point of the
// three-pass split: the serial part now operates on block sums, not on elements.
//
// In-place is safe because each chunk is fully loaded into groupshared before anything is written back,
// and chunks are visited in order by the one workgroup that owns them.



groupshared uint gScanA[SCAN_BLOCK];
groupshared uint gScanB[SCAN_BLOCK];

[shader("compute")]
[numthreads(SCAN_BLOCK, 1, 1)]
void ComputeMain(uint Lane : SV_GroupIndex)
{
    const uint N         = Pass().CountPtr[0];
    const uint NumBlocks = (N + SCAN_BLOCK - 1u) / SCAN_BLOCK;

    uint Running = 0u;   // uniform: every lane accumulates the same chunk totals

    for (uint Base = 0u; Base < NumBlocks; Base += SCAN_BLOCK)
    {
        const uint Idx   = Base + Lane;
        const uint Value = (Idx < NumBlocks) ? Pass().BlockSums[Idx] : 0u;

        gScanA[Lane] = Value;
        GroupMemoryBarrierWithGroupSync();

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

        if (Idx < NumBlocks)
        {
            Pass().BlockSums[Idx] = Running + gScanA[Lane] - Value;   // inclusive -> exclusive
        }
        Running += gScanA[SCAN_BLOCK - 1u];

        // All lanes have read gScanA above before the next chunk overwrites it.
        GroupMemoryBarrierWithGroupSync();
    }

    if (Lane == 0u)
    {
        Pass().BlockSums[NumBlocks] = Running;   // total meshlets across every surviving instance
    }
}
