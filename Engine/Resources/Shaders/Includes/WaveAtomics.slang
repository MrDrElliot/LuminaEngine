#pragma once

// One atomic per wave, or per distinct key in it, each lane getting what its own InterlockedAdd would have seen under one valid lane order.

// Every active lane must name the same Counter.
uint WaveInterlockedAdd(uint* Counter, uint Value)
{
    // All taken before the branch, and the result read back from the leader by index, since lanes that diverged earlier may rejoin at the merge.
    const uint Total  = WaveActiveSum(Value);
    const uint Prefix = WavePrefixSum(Value);
    const uint Leader = WaveActiveMin(WaveGetLaneIndex());
    uint Old = 0u;
    if (WaveGetLaneIndex() == Leader)
    {
        InterlockedAdd(Counter[0], Total, Old);
    }
    return WaveReadLaneAt(Old, Leader) + Prefix;
}

// Every active lane must name the same Counter.
void WaveInterlockedAddNoReturn(uint* Counter, uint Value)
{
    const uint Total  = WaveActiveSum(Value);
    const uint Leader = WaveActiveMin(WaveGetLaneIndex());
    if (WaveGetLaneIndex() == Leader && Total != 0u)
    {
        InterlockedAdd(Counter[0], Total);
    }
}

// Lanes may name different counters, each picked by Key, so a wave that mostly shares one pays one atomic per key it holds.
uint WaveInterlockedAddKeyed(uint Key, uint* Counter, uint Value)
{
    uint Result = 0u;
    [loop]
    for (;;)
    {
        if (Key == WaveReadLaneFirst(Key))
        {
            Result = WaveInterlockedAdd(Counter, Value);
            break;
        }
    }
    return Result;
}
