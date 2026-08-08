#pragma once

// The GPU-side append protocol, in one place.
//
// Every producer in the renderer does the same three things: bump a cursor other lanes are bumping,
// decide whether what it reserved fits the region it was handed, and either write or drop. That was six
// separate copies -- the meshlet cull's emit and defer, the block builder, the instance cull, terrain,
// material tiles -- each with its own comparison and its own idea of what "full" means. An unbounded
// atomic used directly as a write index is the shape that took out the meshlet defer list, and two of
// those six carry a comment saying exactly that, written after the fact.
//
// NOT a struct holding the buffer pointers, deliberately. A struct containing a PhysicalStorageBuffer
// pointer must never be a local Slang has to materialize: it emits an OpVariable carrying neither
// Aliased nor Restrict, which is invalid SPIR-V that every driver accepts and then optimizes against
// aliasing rules nothing established -- surfacing frames later as a device-lost with no shader
// attribution. So the cursor arrives as a bare pointer parameter, which Slang inlines away. Every shape
// used below -- plain pointer, pointer arithmetic into an array, and the address of a field inside a
// BDA struct -- was checked with spirv-val before this file existed, not assumed.

// "The region could not fit this reservation." Distinct from any real slot: a 2^32-1 entry buffer is not
// reachable, and every caller tests for it before writing.
static const uint kAppendFull = ~0u;

/**
 * Reserves Count contiguous entries at Cursor and returns the first as an index into the buffer Base
 * belongs to, or kAppendFull when the region cannot hold them.
 *
 * All-or-nothing: a run that does not fit entirely is refused rather than truncated. Use
 * AppendReserveClamped where a short write is meaningful.
 *
 * The cursor deliberately keeps advancing past Capacity on an overflow frame. That is what makes the
 * value read back afterwards DEMAND rather than what fit, which is what the next allocation is sized
 * from -- so an undersized buffer corrects itself instead of silently staying small.
 */
uint AppendReserve(uint* Cursor, uint Capacity, uint Base, uint Count = 1u)
{
    uint Old;
    InterlockedAdd(Cursor[0], Count, Old);

    // Written as a subtraction rather than `Old + Count <= Capacity` so it cannot wrap: Old is unbounded
    // by design (see above), and on a badly overflowed frame the sum is exactly what would carry.
    const bool bFits = (Old < Capacity) && ((Capacity - Old) >= Count);
    return bFits ? (Base + Old) : kAppendFull;
}

/**
 * Same reservation, but takes whatever is left instead of refusing the whole run. Returns the first slot
 * and writes how many of Count were granted -- 0 when the region was already full, in which case the
 * returned slot must not be written.
 */
uint AppendReserveClamped(uint* Cursor, uint Capacity, uint Base, uint Count, out uint Granted)
{
    uint Old;
    InterlockedAdd(Cursor[0], Count, Old);

    Granted = (Old < Capacity) ? min(Count, Capacity - Old) : 0u;
    return Base + Old;
}
