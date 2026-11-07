#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// One group turns the sun grid's per-cell caster counts into each cell's end in place, and writes the total past the last cell.
#define SCAN_THREADS 256u

// Mirror of Lumina::FSunDFShadowScanArgs.
struct FPushConstants
{
    GPUSpan<uint> CellCounts;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void ComputeMain(uint ThreadIndex : SV_GroupIndex)
{
    const uint NumCells  = Pass().CellCounts.Count - 1u;
    const uint PerThread = (NumCells + SCAN_THREADS - 1u) / SCAN_THREADS;
    const uint First     = min(ThreadIndex * PerThread, NumCells);
    const uint Last      = min(First + PerThread, NumCells);

    uint Local = 0u;
    for (uint i = First; i < Last; ++i)
    {
        Local += Pass().CellCounts.Get(i);
    }

    uint2 Total;
    uint Running = GroupExclusiveScan(uint2(Local, 0u), ThreadIndex, SCAN_THREADS, Total).x;
    for (uint i = First; i < Last; ++i)
    {
        Running += Pass().CellCounts.Get(i);
        Pass().CellCounts.Set(i, Running);
    }

    if (ThreadIndex == 0u)
    {
        Pass().CellCounts.Set(NumCells, Total.x);
    }
}
