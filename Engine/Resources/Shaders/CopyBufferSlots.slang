#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Copies the listed fixed-size elements between two buffers of the same layout, one 16-byte word per thread.

struct FPushConstants
{
    GPUSpan<uint4> Source;
    GPUSpan<uint4> Dest;
    // Transform stream words are read by their head slot, which is the only one of the run holding a transform.
    GPUSpan<uint>  Slots;
    uint           WordsPerElement;
    uint           _Pad0;
    uint           _Pad1;
    uint           _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint Thread = (GroupID.x + GroupID.y * MAX_DISPATCH_AXIS) * 64u + LocalID.x;
    const uint Words  = Pass().WordsPerElement;
    const uint Entry  = Thread / Words;
    if (Entry >= Pass().Slots.Count)
    {
        return;
    }

    // A dropped stream word masks to a slot past every buffer, so the bound below discards it.
    const uint Slot = Pass().Slots.Get(Entry) & TRANSFORM_STREAM_SLOT_MASK;
    const uint Word = Slot * Words + Thread % Words;
    if (Word < Pass().Source.Count && Word < Pass().Dest.Count)
    {
        Pass().Dest.Set(Word, Pass().Source.Get(Word));
    }
}
