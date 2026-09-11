#pragma once

// Mirror of RHI::TGPUSpan<T>. The element count rides in the same 16 bytes as the pointer, so a pass
// cannot hand a shader an array without also handing it the bound, and the two can never be authored
// from different places. Every out-of-bounds read this renderer has page-faulted on came from a count
// that lived somewhere other than the address it was meant to bound.
//
// ACCESS IS THROUGH THE FREE FUNCTIONS BELOW, which take a GPUSpan<T>* and never load the struct by
// value. A struct carrying a PhysicalStorageBuffer pointer must never be materialized as a Slang local:
// it emits an OpVariable with neither Aliased nor Restrict, which is invalid SPIR-V that every driver
// accepts and then optimizes against aliasing rules nothing established. Reading Span.Count or
// Span.Data straight off the pass pointer is fine, those are scalar loads through an access chain.
struct GPUSpan<T>
{
    T*   Data;
    uint Count;
    uint _Pad;
};

bool SpanIsEmpty<T>(GPUSpan<T>* Span)
{
    return Span.Count == 0u;
}

// The guard to reach for when the caller needs to branch on range rather than absorb a zero.
bool SpanContains<T>(GPUSpan<T>* Span, uint Index)
{
    return Index < Span.Count;
}

// Reads past the end resolve to zero rather than to whatever the allocator has since put there.
// Loads sizeof(T) whatever the caller then extracts, so one field of a large element wants SpanAt below.
T SpanGet<T>(GPUSpan<T>* Span, uint Index)
{
    if (Index >= Span.Count)
    {
        T Zero = {};
        return Zero;
    }
    return Span.Data[Index];
}

// Same bound, same zero on reject, for the loads that want the wide path.
T SpanGetAligned<T>(GPUSpan<T>* Span, uint Index)
{
    if (Index >= Span.Count)
    {
        T Zero = {};
        return Zero;
    }
    return loadAligned<16>(Span.Data + Index);
}

// Stores past the end are dropped. A dropped store is a missing draw; an unchecked one is a device loss.
void SpanSet<T>(GPUSpan<T>* Span, uint Index, T Value)
{
    if (Index < Span.Count)
    {
        Span.Data[Index] = Value;
    }
}

// Escape hatch for atomics, in-place field writes, and field reads that must stay narrow (a field read
// through this compiles to an access chain and a load of that field alone). Bound the index first.
__generic<T> T* SpanAt(GPUSpan<T>* Span, uint Index)
{
    return Span.Data + Index;
}
