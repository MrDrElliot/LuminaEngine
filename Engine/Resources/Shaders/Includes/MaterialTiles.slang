#pragma once

// Shared contract for the deferred material pass' tile binning and material-slot depth encoding.
//
// The material pass rejects foreign pixels with a hardware DEPTH_EQUAL test: MaterialDepth.slang stamps
// every covered pixel with its owning slot encoded as a depth value, and each material's tile draw emits
// its quad at that same encoded depth. The comparison is bit-exact float equality, so the encoding MUST be
// byte-identical on both sides -- which is why it lives here and is called, never re-derived. (The same
// mistake in a duplicated push-constant block is what made the prefix scan read its own output in place.)
//
// The encode is exact in fp32 by construction: a uint -> float conversion of a value <= 65535 is exact, and
// the scale is a power of two, so the multiply only decrements the exponent. Both the per-pixel stamp and
// the per-draw quad therefore produce the identical bit pattern for a given slot.

// Square tile edge in pixels. Coarse enough to keep the tile count and per-tile bitmask small, fine enough
// that over-inclusion (a tile binned for a material that only clips its corner) stays cheap.
#define MATERIAL_TILE_SIZE_PX 16u

// Slot 0 encodes to 1/65536 and MaterialDepth clears to 0, so "no material owns this pixel" can never
// collide with a real slot. Cap chosen so (Slot + 1) * scale stays inside the [0,1] depth range with room
// to spare, and so the compacted tile list (TotalTiles entries per slot) stays a few MB at 4K.
#define MATERIAL_SLOT_DEPTH_SCALE (1.0f / 65536.0f)
#define MATERIAL_MAX_SLOTS        64u

float MaterialSlotToDepth(uint Slot)
{
    return (float)(Slot + 1u) * MATERIAL_SLOT_DEPTH_SCALE;
}
