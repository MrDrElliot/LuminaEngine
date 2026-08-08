#pragma once

#include "Array.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    /**
     * Allocates slots out of a fixed-capacity range, always handing out the lowest free one.
     *
     * Replaces the bit-at-a-time linear scans this engine had grown two of. Occupancy lives in 64-bit
     * words scanned with a count-trailing-zeros, so a full word of allocated slots costs one test
     * instead of sixty-four, and a search hint means the common case never rescans the full region at
     * all. At the bindless heap's 8192 slots that is 128 word tests worst case rather than 8192 bit
     * tests, per allocation, under the heap lock.
     *
     * Lowest-free-first is not a strategy flag, it falls out of scanning from the least significant bit
     * -- and it is what keeps the occupied range tight, so anything that sweeps or uploads over the
     * region stays proportional to what is live rather than to the capacity.
     *
     * DELIBERATELY NOT THREAD-SAFE. Every caller already holds a lock covering the wider structure this
     * indexes (the RHI's heap mutex guards the descriptors these slots name, and the allocation has to
     * be atomic with the descriptor write, not merely with itself). A second lock in here would be
     * uncontended overhead and an invitation to a lock-ordering bug.
     *
     * Single slots only. Esoterica's equivalent allocates contiguous RANGES because it suballocates a
     * GPU page table; nothing here does -- textures, samplers and storage images each take exactly one
     * slot, and the per-frame bone and pre-skin arenas are bump allocators that reset. Range support is
     * a straightforward extension of the same scan if a caller ever needs it.
     */
    class FHandleAllocator
    {
    public:

        static constexpr uint32 kInvalidHandle = ~0u;

        FHandleAllocator() = default;
        explicit FHandleAllocator(uint32 InCapacity) { Reset(InCapacity); }

        void Reset(uint32 InCapacity)
        {
            Capacity     = InCapacity;
            NumAllocated = 0;
            HintWord     = 0;
            HighWater    = 0;

            Words.assign((InCapacity + 63u) / 64u, 0ull);

            // The tail of the last word addresses slots past Capacity. Marking them allocated up front is
            // what lets the scan below trust a whole word without a per-bit capacity compare -- they can
            // never come back free, so they can never be handed out.
            const uint32 Tail = InCapacity & 63u;
            if (Tail != 0u && !Words.empty())
            {
                Words.back() = ~((1ull << Tail) - 1ull);
            }
        }

        /** Lowest free slot, or kInvalidHandle when the region is full. */
        uint32 Alloc()
        {
            // Everything below HintWord is full: Alloc only moves the hint forward past words it found
            // full, and Free pulls it back to whatever it just released. So this never skips a free slot
            // and never needs a wrap-around second pass.
            for (uint32 w = HintWord; w < (uint32)Words.size(); ++w)
            {
                const uint64 Free = ~Words[w];
                if (Free == 0ull)
                {
                    continue;
                }

                const uint32 Bit  = (uint32)Math::CountTrailingZeros64(Free);
                const uint32 Slot = w * 64u + Bit;

                Words[w] |= (1ull << Bit);
                HintWord  = w;
                ++NumAllocated;
                HighWater = Math::Max(HighWater, Slot + 1u);
                return Slot;
            }

            HintWord = (uint32)Words.size();
            return kInvalidHandle;
        }

        /**
         * Claims one specific slot. Returns false if it was out of range or already taken.
         *
         * For identities published outside the allocator, which must be repointed rather than freed and
         * reallocated -- a bindless index already baked into recorded command buffers has to keep naming
         * the same slot even as the resource behind it changes.
         */
        bool MarkAllocated(uint32 Slot)
        {
            if (Slot >= Capacity)
            {
                return false;
            }

            const uint64 Mask = 1ull << (Slot & 63u);
            uint64&      Word = Words[Slot >> 6];
            if ((Word & Mask) != 0ull)
            {
                return false;
            }

            Word     |= Mask;
            ++NumAllocated;
            HighWater = Math::Max(HighWater, Slot + 1u);
            return true;
        }

        /** Releases a slot. Freeing one that is already free is a no-op, not an error. */
        void Free(uint32 Slot)
        {
            if (Slot >= Capacity)
            {
                return;
            }

            const uint64 Mask = 1ull << (Slot & 63u);
            uint64&      Word = Words[Slot >> 6];
            if ((Word & Mask) == 0ull)
            {
                return;
            }

            Word &= ~Mask;
            --NumAllocated;

            // Restores the "everything below the hint is full" invariant Alloc relies on.
            HintWord = Math::Min(HintWord, Slot >> 6);
        }

        NODISCARD bool IsAllocated(uint32 Slot) const
        {
            return Slot < Capacity && (Words[Slot >> 6] & (1ull << (Slot & 63u))) != 0ull;
        }

        /**
         * Visits every allocated slot in ascending order, skipping empty words wholesale.
         *
         * The reason to prefer this over an index loop with IsAllocated: a sparsely populated heap costs
         * one test per empty word rather than one per empty slot.
         */
        template<typename TFunc>
        void ForEachAllocated(TFunc&& Func) const
        {
            for (uint32 w = 0; w < (uint32)Words.size(); ++w)
            {
                uint64 Bits = Words[w];

                // The last word's padding is marked allocated; mask it back off so it is never visited.
                if (w == (uint32)Words.size() - 1u)
                {
                    const uint32 Tail = Capacity & 63u;
                    if (Tail != 0u)
                    {
                        Bits &= (1ull << Tail) - 1ull;
                    }
                }

                while (Bits != 0ull)
                {
                    const uint32 Bit = (uint32)Math::CountTrailingZeros64(Bits);
                    Bits &= Bits - 1ull;                    // clear lowest set bit
                    Func(w * 64u + Bit);
                }
            }
        }

        NODISCARD uint32 GetCapacity() const     { return Capacity; }
        NODISCARD uint32 GetNumAllocated() const { return NumAllocated; }

        /** One past the highest slot ever allocated. Monotonic, so an upper bound rather than a live max. */
        NODISCARD uint32 GetHighWaterMark() const { return HighWater; }

    private:

        TVector<uint64> Words;
        uint32          Capacity     = 0;
        uint32          NumAllocated = 0;
        // Lowest word that might contain a free slot; see Alloc.
        uint32          HintWord     = 0;
        uint32          HighWater    = 0;
    };
}
