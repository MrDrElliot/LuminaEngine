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
    // Pending flag rather than a break: AMD's compiler sinks a broken-out body past the loop exit, running the add once over every key.
    uint Result = 0u;
    bool bPending = true;
    [loop]
    while (WaveActiveAnyTrue(bPending))
    {
        if (bPending)
        {
            if (Key == WaveReadLaneFirst(Key))
            {
                Result = WaveInterlockedAdd(Counter, Value);
                bPending = false;
            }
        }
    }
    return Result;
}

// Every active lane must name the same Counter and Capacity. Lanes fit in lane order, so the claimed units stay contiguous and a refused lane never moves the counter.
bool WaveClaimWithin(uint* Counter, uint Capacity, uint Count, out uint Old)
{
    const uint Exclusive = WavePrefixSum(Count);
    const uint Inclusive = Exclusive + Count;
    const uint Leader    = WaveActiveMin(WaveGetLaneIndex());

    uint Current = 0u;
    if (WaveGetLaneIndex() == Leader)
    {
        InterlockedAdd(Counter[0], 0u, Current);
    }
    Current = WaveReadLaneAt(Current, Leader);

    // Wave-uniform exit, and only another wave's claim can fail the exchange, so the counter rises until it succeeds or nothing fits.
    bool bFits    = false;
    bool bPending = true;
    [loop]
    while (bPending)
    {
        const uint Room  = Current < Capacity ? Capacity - Current : 0u;
        bFits            = Inclusive <= Room;
        const uint Claim = WaveActiveMax(bFits ? Inclusive : 0u);

        uint Seen = Current;
        if (Claim != 0u && WaveGetLaneIndex() == Leader)
        {
            InterlockedCompareExchange(Counter[0], Current, Current + Claim, Seen);
        }
        Seen     = WaveReadLaneAt(Seen, Leader);
        bPending = Claim != 0u && Seen != Current;
        Current  = Seen;
    }

    Old = Current + Exclusive;
    return bFits;
}

// Lanes may name different counters, each picked by Key, with one capacity per key.
bool WaveClaimWithinKeyed(uint Key, uint* Counter, uint Capacity, uint Count, out uint Old)
{
    Old = 0u;
    bool bResult  = false;
    bool bPending = true;
    [loop]
    while (WaveActiveAnyTrue(bPending))
    {
        if (bPending)
        {
            if (Key == WaveReadLaneFirst(Key))
            {
                bResult  = WaveClaimWithin(Counter, Capacity, Count, Old);
                bPending = false;
            }
        }
    }
    return bResult;
}
