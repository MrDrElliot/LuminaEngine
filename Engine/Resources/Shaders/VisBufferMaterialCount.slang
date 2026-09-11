#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Deferred material classification step 1, producing the per-material pixel counts the prefix sum scans.

struct FMaterialCountArgs
{
    GPUSpan<uint> Counts;           // zeroed before this pass; its count is the slot count
    GPUSpan<uint> SlotByMaterial;   // global MaterialIndex -> dense slot (~0u = no deferred draw)
    uint  VisBufferIndex;
    uint  ScreenW;
    uint  ScreenH;
    uint  DrawListCount;
};
FMaterialCountArgs* Pass() { return GetPassArgs<FMaterialCountArgs>(); }

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint Slot = ~0u;
    if (DTid.x < Pass().ScreenW && DTid.y < Pass().ScreenH)
    {
        const uint Raw = LoadTexture2DUInt(Pass().VisBufferIndex, int2(DTid.xy));

        // Scatter resolves and aggregates identically, or it fills a run sized differently from this.
        const uint Resolved = ResolveVisMaterialSlot(Raw, Pass().DrawListCount, &Pass().SlotByMaterial);
        if (SpanContains(&Pass().Counts, Resolved))
        {
            Slot = Resolved;
        }
    }

    if (Slot == ~0u)
    {
        return;
    }

    // One pass per distinct slot among the lanes still here, which for neighboring pixels is one or two.
    while (true)
    {
        const uint Candidate = WaveReadLaneFirst(Slot);
        if (Candidate == Slot)
        {
            const uint Count = WaveActiveCountBits(true);
            if (WaveIsFirstLane())
            {
                InterlockedAdd(SpanAt(&Pass().Counts, Candidate)[0], Count);
            }
            break;
        }
    }
}
