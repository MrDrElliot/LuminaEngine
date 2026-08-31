#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Deferred material classification step 3, writing each classified pixel into its material's run.

struct FMaterialScatterArgs
{
    uint* Cursors;              // uint[NumSlots], seeded to Starts by the prefix sum
    uint* PixelList;            // uint[PixelListCapacity], packed 16:16 screen position
    uint* SlotByMaterial;
    uint  VisBufferIndex;
    uint  ScreenW;
    uint  ScreenH;
    uint  DrawListCount;
    uint  SlotByMaterialCount;
    uint  NumSlots;
    uint  PixelListCapacity;
    uint  _Pad0;
};
FMaterialScatterArgs* Pass() { return GetPassArgs<FMaterialScatterArgs>(); }

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint Slot = ~0u;
    if (DTid.x < Pass().ScreenW && DTid.y < Pass().ScreenH)
    {
        const uint Raw = LoadTexture2DUInt(Pass().VisBufferIndex, int2(DTid.xy));

        // The resolve and aggregation scope VisBufferMaterialCount used, so the run sized there fits.
        const uint Resolved = ResolveVisMaterialSlot(Raw, Pass().DrawListCount,
                                                     Pass().SlotByMaterial, Pass().SlotByMaterialCount);
        if (Resolved < Pass().NumSlots)
        {
            Slot = Resolved;
        }
    }

    if (Slot == ~0u)
    {
        return;
    }

    // The lanes sharing a material reserve one contiguous range, then each writes at its rank inside it.
    while (true)
    {
        const uint Candidate = WaveReadLaneFirst(Slot);
        if (Candidate == Slot)
        {
            const uint Rank  = WavePrefixCountBits(true);
            const uint Count = WaveActiveCountBits(true);

            uint Base = 0u;
            if (WaveIsFirstLane())
            {
                InterlockedAdd(Pass().Cursors[Candidate], Count, Base);
            }

            // The reserving lane is the first one still active here, which is who broadcasts.
            Base = WaveReadLaneFirst(Base);

            // Bounded despite the counts coming from this same screen, since the failure is a stray write.
            const uint Index = Base + Rank;
            if (Index < Pass().PixelListCapacity)
            {
                Pass().PixelList[Index] = PackVisPixel(DTid.xy);
            }
            break;
        }
    }
}
