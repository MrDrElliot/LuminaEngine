#pragma once
#include "WaveAtomics.slang"

// Bare pointer parameters, since a local struct holding a PhysicalStorageBuffer pointer emits invalid SPIR-V without Aliased or Restrict.

static const uint kAppendFull = ~0u;

// All-or-nothing, one atomic per wave so active lanes must share Cursor, which keeps advancing past Capacity so readback reports demand.
uint AppendReserve(uint* Cursor, uint Capacity, uint Base, uint Count = 1u)
{
    const uint Old = WaveInterlockedAdd(Cursor, Count);

    // Subtracting cannot wrap the way Old + Count can, since Old is unbounded by design.
    const bool bFits = (Old < Capacity) && ((Capacity - Old) >= Count);
    return bFits ? (Base + Old) : kAppendFull;
}

// Granted is 0 when the region was already full, and then the returned slot must not be written.
uint AppendReserveClamped(uint* Cursor, uint Capacity, uint Base, uint Count, out uint Granted)
{
    uint Old;
    InterlockedAdd(Cursor[0], Count, Old);

    Granted = (Old < Capacity) ? min(Count, Capacity - Old) : 0u;
    return Base + Old;
}

// Bounded by the target's own count rather than a number from elsewhere.
uint AppendReserve<T>(GPUSpan<uint> Cursor, GPUSpan<T> Target, uint Base, uint Count = 1u)
{
    return AppendReserve(Cursor.At(0), Target.Count, Base, Count);
}

uint AppendReserveClamped<T>(GPUSpan<uint> Cursor, GPUSpan<T> Target, uint Base, uint Count, out uint Granted)
{
    return AppendReserveClamped(Cursor.At(0), Target.Count, Base, Count, Granted);
}
