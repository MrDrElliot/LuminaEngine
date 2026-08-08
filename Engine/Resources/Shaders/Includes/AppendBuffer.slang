#pragma once

// The GPU-side append protocol. Bare pointer parameters rather than a struct holding them: a struct
// carrying a PhysicalStorageBuffer pointer must never be a local Slang has to materialize, because it
// emits an OpVariable with neither Aliased nor Restrict -- invalid SPIR-V that every driver accepts and
// then optimizes against aliasing rules nothing established.

static const uint kAppendFull = ~0u;

// All-or-nothing: a run that does not fit entirely is refused rather than truncated. The cursor keeps
// advancing past Capacity on an overflow frame, which is what makes the value read back afterwards
// DEMAND rather than what fit -- so an undersized buffer corrects itself.
uint AppendReserve(uint* Cursor, uint Capacity, uint Base, uint Count = 1u)
{
    uint Old;
    InterlockedAdd(Cursor[0], Count, Old);

    // A subtraction rather than `Old + Count <= Capacity` so it cannot wrap: Old is unbounded by design.
    const bool bFits = (Old < Capacity) && ((Capacity - Old) >= Count);
    return bFits ? (Base + Old) : kAppendFull;
}

// Takes whatever is left instead of refusing the whole run. Granted is 0 when the region was already
// full, in which case the returned slot must not be written.
uint AppendReserveClamped(uint* Cursor, uint Capacity, uint Base, uint Count, out uint Granted)
{
    uint Old;
    InterlockedAdd(Cursor[0], Count, Old);

    Granted = (Old < Capacity) ? min(Count, Capacity - Old) : 0u;
    return Base + Old;
}
