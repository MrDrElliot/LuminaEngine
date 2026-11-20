#pragma once
#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"

// The dispatch every deferred material template shares, whichever surface it rebuilds.

// Must match FDeferredMaterialPC in SceneRenderer_Lighting.cpp.
struct FDeferredMaterialArgs
{
    FTexture2DUInt VisBuffer;   // 0
    uint           FeedbackPhase;    // 4   which 1/STREAMING_FEEDBACK_WINDOW of the groups report streaming feedback this frame
    uint           NumSlots;         // 8   where the sparse half of Counts and Offsets starts
    FTexture2DUInt SlotImage;   // 12  slot + 1 per pixel from classify
    uint           DrawListCount;    // 16  MeshletDrawList slot bound
    uint           SlotIndex;        // 20
    uint           ScreenW;          // 24
    uint           ScreenH;          // 28
    FRWTexture2D   GBufferA;      // 32  storage-heap slots, channel order per Includes/GBuffer.slang
    FRWTexture2D   GBufferB;      // 36
    FRWTexture2D   GBufferC;      // 40
    FRWTexture2D   GBufferD;      // 44
    FRWTexture2D   Velocity;      // 48  0xFFFFFFFF when temporal AA is off
    uint           _PadVelocity;     // 52
    GPUSpan<uint>  PairList;   // 56  packed tile coordinates grouped by slot, each run starting at Offsets[slot]
    GPUSpan<uint>  Counts;     // 72  dense tiles per slot, then sparse pixels per slot
    GPUSpan<uint>  Offsets;    // 88  run starts in PairList, then in PixelList
    GPUSpan<uint>  PixelList;  // 104 packed pixel coordinates of the pairs too thin to shade as a tile
};
FDeferredMaterialArgs* Pass() { return GetPassArgs<FDeferredMaterialArgs>(); }

// Off without a velocity target, so the motion-vector path and its second vertex-graph run drop out of the register budget.
[vk::constant_id(9)] const uint SPEC_DEFERRED_VELOCITY = 1;

// The pixel this lane shades, folded across Y with the matching unfold in VisBufferMaterialArgs, dense tiles first and listed pixels after.
bool FindDeferredTexel(uint3 GroupID, uint Lane, out uint2 Texel, out uint LinearGroup)
{
    const uint Slot = Pass().SlotIndex;
    LinearGroup = GroupID.y * MAX_DISPATCH_AXIS + GroupID.x;
    const uint DenseTiles = Pass().Counts.Get(Slot);

    if (LinearGroup >= DenseTiles)
    {
        const uint SparseRun = Pass().NumSlots + Slot;
        const uint Entry     = (LinearGroup - DenseTiles) * MATERIAL_PIXEL_GROUP_SIZE + Lane;
        Texel = uint2(0u, 0u);
        if (Entry >= Pass().Counts.Get(SparseRun))
        {
            return false;
        }
        Texel = UnpackVisPixel(Pass().PixelList.Get(Pass().Offsets.Get(SparseRun) + Entry));
        return true;
    }

    const uint2 Tile = UnpackVisPixel(Pass().PairList.Get(Pass().Offsets.Get(Slot) + LinearGroup));
    Texel = Tile * MATERIAL_CLASSIFY_TILE + uint2(Lane % MATERIAL_CLASSIFY_TILE, Lane / MATERIAL_CLASSIFY_TILE);
    if (Texel.x >= Pass().ScreenW || Texel.y >= Pass().ScreenH)
    {
        return false;
    }

    // A tile is shared with other materials, so the slot classify stored keeps only this one's pixels.
    return Pass().SlotImage.Load(int2(Texel)) == Slot + 1u;
}

void WriteDeferredGBuffer(uint2 Texel, FGBufferData G)
{
    const FGBufferPacked P = PackGBuffer(G);
    Pass().GBufferA[int2(Texel)] = P.A;
    Pass().GBufferB[int2(Texel)] = P.B;
    Pass().GBufferC[int2(Texel)] = P.C;
    Pass().GBufferD[int2(Texel)] = P.D;
}
