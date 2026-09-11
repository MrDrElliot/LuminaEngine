#pragma once

#include "RHI.h"

namespace Lumina::RHI
{
    // What a pass hands a shader instead of a bare device address. The element count travels in the same
    // 16 bytes as the pointer and is derived from the allocation that produced it, so the two cannot be
    // authored separately and cannot disagree. Every out-of-bounds read this renderer has page-faulted on
    // came from a count that lived somewhere else than the address it was supposed to bound.
    //
    // Mirrors GPUSpan<T> in Includes/GPUSpan.slang.
    template<typename T>
    struct TGPUSpan
    {
        GPUPtr Address = 0;
        uint32 Count   = 0;
        uint32 Pad     = 0;

        TGPUSpan() = default;

        // The whole allocation, counted in elements. Truncates rather than rounding up: a partial trailing
        // element is not addressable, and a count that claims it is what walks off the end.
        TGPUSpan(const FGPUAllocation& Allocation)
            : Address(Allocation.Gpu)
            , Count((uint32)Math::Min<uint64>(Allocation.Size / sizeof(T), 0xFFFFFFFFull))
        {
        }

        // A prefix of the allocation. Clamped to what the allocation actually holds, so a caller's stale
        // element count cannot widen the span past its memory.
        TGPUSpan(const FGPUAllocation& Allocation, uint32 InCount)
            : Address(Allocation.Gpu)
            , Count(Math::Min(InCount, (uint32)Math::Min<uint64>(Allocation.Size / sizeof(T), 0xFFFFFFFFull)))
        {
        }

        // A transient copy, which reports its extent as a byte range rather than as an allocation.
        TGPUSpan(const FGPURange& Range)
            : Address(Range.Address)
            , Count((uint32)Math::Min<uint64>(Range.Size / sizeof(T), 0xFFFFFFFFull))
        {
        }

        // A slice starting ElementOffset in, for the buffers that pack several arrays into one allocation.
        static TGPUSpan Slice(const FGPUAllocation& Allocation, uint64 ElementOffset, uint32 InCount)
        {
            const uint64 Available = Allocation.Size / sizeof(T);
            if (Allocation.Gpu == 0 || ElementOffset >= Available)
            {
                return TGPUSpan();
            }

            TGPUSpan Out;
            Out.Address = Allocation.Gpu + ElementOffset * sizeof(T);
            Out.Count   = (uint32)Math::Min<uint64>(InCount, Available - ElementOffset);
            return Out;
        }

        // For the handful of spans a compute pass writes into a region the CPU only knows by address,
        // such as a counter packed at a fixed offset of a shared buffer.
        static TGPUSpan FromAddress(GPUPtr InAddress, uint32 InCount)
        {
            TGPUSpan Out;
            Out.Address = InAddress;
            Out.Count   = InAddress != 0 ? InCount : 0u;
            return Out;
        }

        bool IsEmpty() const { return Address == 0 || Count == 0; }

        explicit operator bool() const { return !IsEmpty(); }
    };

    static_assert(sizeof(TGPUSpan<uint32>) == 16, "TGPUSpan must match GPUSpan<T> in GPUSpan.slang");
}
