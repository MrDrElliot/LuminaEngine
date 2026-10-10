#pragma once

// A 1024-thread group at wave4, the floor for a device that cannot pin compute to 32 lanes.
#define GROUP_SCAN_MAX_WAVES 256u

groupshared uint2 gsGroupScanWaveTotals[GROUP_SCAN_MAX_WAVES];

// Exclusive prefix sum of two counters across the workgroup. Every thread must call it, since it synchronizes the group.
uint2 GroupExclusiveScan(uint2 Value, uint ThreadIndex, uint GroupThreads, out uint2 GroupTotal)
{
    const uint Lanes = WaveGetLaneCount();
    const uint Wave  = ThreadIndex / Lanes;

    const uint2 WavePrefix = WavePrefixSum(Value);
    const uint2 WaveTotal  = WaveActiveSum(Value);
    if (WaveIsFirstLane())
    {
        gsGroupScanWaveTotals[Wave] = WaveTotal;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint NumWaves = (GroupThreads + Lanes - 1u) / Lanes;
    uint2 WaveBase = uint2(0u, 0u);
    GroupTotal     = uint2(0u, 0u);
    for (uint w = 0u; w < NumWaves; ++w)
    {
        const uint2 Total = gsGroupScanWaveTotals[w];
        WaveBase   += (w < Wave) ? Total : uint2(0u, 0u);
        GroupTotal += Total;
    }
    return WaveBase + WavePrefix;
}
