#include "Includes/SceneGlobals.slang"

// Writes compactly staged elements to their slots, so a scattered dirty set uploads without one copy region per run.

struct FScatterUploadArgs
{
    GPUSpan<uint>  Slots;     // destination element index per staged element
    GPUSpan<uint4> Source;    // staged elements back to back, ElementWords each
    GPUSpan<uint4> Dest;      // the retained buffer, indexed in elements of ElementWords
    uint  Count;
    uint  ElementWords;
    uint  _Pad0;
    uint  _Pad1;
};
FScatterUploadArgs* Pass() { return GetPassArgs<FScatterUploadArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint Index = DTid.x;
    if (Index >= Pass().Count || !SpanContains(&Pass().Slots, Index))
    {
        return;
    }

    const uint Words = Pass().ElementWords;
    const uint Slot  = SpanGet(&Pass().Slots, Index);
    for (uint Word = 0u; Word < Words; ++Word)
    {
        const uint DestIndex = Slot * Words + Word;
        if (SpanContains(&Pass().Dest, DestIndex))
        {
            SpanSet(&Pass().Dest, DestIndex, SpanGet(&Pass().Source, Index * Words + Word));
        }
    }
}
