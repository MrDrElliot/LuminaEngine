#include "Includes/ScanPrefixArgs.slang"

// Pass 3 of 3: add each block's global offset back onto its block-local prefix, turning pass 1's output
// into the true exclusive prefix the meshlet cull binary-searches. One thread per element, fully parallel.
//
// Dispatched with the same indirect args as pass 1 (identical domain), so the two stay in lockstep.



[shader("compute")]
[numthreads(SCAN_BLOCK, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupIndex)
{
    const uint N     = Pass().CountPtr[0];
    const uint Block = GroupID.x + GroupID.y * 65535u;
    const uint Idx   = Block * SCAN_BLOCK + Lane;

    if (Idx < N)
    {
        Pass().Prefix[Idx] += Pass().BlockSums[Block];
    }

    // Sentinel entry. Not read by ResolveInstanceFromFlatIndex (its Hi bound is exclusive and never
    // dereferenced), but the array's contract says [Count] is the total and captures are easier to read
    // when it holds one.
    if (Idx == 0u)
    {
        const uint NumBlocks = (N + SCAN_BLOCK - 1u) / SCAN_BLOCK;
        Pass().Prefix[N] = Pass().BlockSums[NumBlocks];
    }
}
