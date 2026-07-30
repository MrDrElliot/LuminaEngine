#include "Includes/SceneGlobals.slang"
#include "Includes/MaterialTiles.slang"

// Material depth + tile binning for the deferred VisBuffer material pass. A fullscreen depth-only raster
// pass: resolve each covered pixel's owning material slot from the visibility buffer, stamp that slot into
// MaterialDepth as an encoded depth value, and OR the slot's bit into the pixel's tile bitmask.
//
// The depth stamp is what lets the material passes reject foreign pixels in FIXED-FUNCTION hardware: each
// material's tile draw emits its quad at its own encoded depth with DEPTH_EQUAL, so non-owned pixels die
// before a pixel shader is ever launched, and whole depth blocks are rejected at coarse rate. The previous
// scheme wrote the slot into a plain buffer and had every material shader load-and-`discard` per pixel,
// which both paid the load and defeated early-Z for the entire draw (a shader that can discard cannot have
// its depth/coverage resolved up front).
//
// The bitmask is the dedupe structure BuildMaterialTileList compacts into per-slot tile lists. Conservative
// by construction: a tile's bit is set if ANY of its pixels belong to the slot, so a material's binned draw
// can never miss one of its pixels.

struct FMaterialDepthArgs
{
    uint  VisBufferIndex;       // R32_UINT visibility target (VisID + 1; 0 = empty/background)
    uint  TileCountX;           // tiles across the screen
    uint  TileSizePx;           // square tile edge in pixels (MATERIAL_TILE_SIZE_PX)
    uint  TileWordCount;        // uint words per tile bitmask = ceil(NumSlots / 32)
    uint  DrawListCount;        // MeshletDrawList slot bound (the whole allocation, see MeshletDrawListCapacity)
    uint  SlotByMaterialCount;  // entries in SlotByMaterial (guards the lookup)
    uint  _Pad0;
    uint  _Pad1;
    uint* TileBits;             // uint[TileCount * TileWordCount], cleared to 0 before this pass
    uint* SlotByMaterial;       // uint[SlotByMaterialCount]: global MaterialIndex -> dense slot (~0u = none)
};
FMaterialDepthArgs* Pass() { return GetPassArgs<FMaterialDepthArgs>(); }

[shader("fragment")]
float FragmentMain(float4 SvPosition : SV_Position) : SV_Depth
{
    const int2 Texel = int2(SvPosition.xy);

    const uint Raw = LoadTexture2DUInt(Pass().VisBufferIndex, Texel);
    if (Raw == 0u)
    {
        // Background (or terrain, which stamps 0 where it wins depth). Leave MaterialDepth at its cleared
        // 0.0, which no slot encodes to, so no material draw can claim this pixel.
        discard;
        return 0.0f;
    }

    // Fault-safe resolve: clamp every index so a stale draw-list gap can never page-fault a speculated
    // buffer-device-address load.
    const uint SlotCount   = Pass().DrawListCount;
    const uint InstCount   = Scene().CullData.InstanceNum;
    const uint VisID       = Raw - 1u;
    const uint MeshletSlot = VisID >> 7u;

    bool bValid           = (SlotCount != 0u) && (MeshletSlot < SlotCount);
    const uint SafeSlot   = bValid ? MeshletSlot : 0u;

    const FMeshletDraw Draw = MeshletDrawList()[SafeSlot];
    // Tag check turns "slot inside the allocation but never written this frame" from a silently resolved
    // stale instance into a background pixel.
    bValid                = bValid && Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) && (Draw.InstanceID < InstCount);
    const uint SafeInst   = (Draw.InstanceID < InstCount) ? Draw.InstanceID : 0u;

    const uint MaterialIndex = GetInstance(SafeInst).MaterialIndex;

    // Every material instance of a master maps to the master's dense slot, so instances sharing a shader
    // share one tile draw and one depth value; the shader still reads each pixel's own MaterialIndex for
    // its uniforms.
    const uint Slot = (bValid && MaterialIndex < Pass().SlotByMaterialCount)
        ? Pass().SlotByMaterial[MaterialIndex]
        : 0xFFFFFFFFu;

    // ~0u covers both an invalid resolve and a material with no deferred draw (no DeferredShader, or a slot
    // past MATERIAL_MAX_SLOTS). Discard BEFORE the atomic: binning a tile for a slot that has no draw would
    // be harmless, but binning one for a garbage slot index would not.
    if (Slot >= MATERIAL_MAX_SLOTS)
    {
        discard;
        return 0.0f;
    }

    const uint TileX   = (uint)Texel.x / Pass().TileSizePx;
    const uint TileY   = (uint)Texel.y / Pass().TileSizePx;
    const uint Tile    = TileY * Pass().TileCountX + TileX;
    const uint WordIdx = Tile * Pass().TileWordCount + (Slot >> 5u);

    InterlockedOr(Pass().TileBits[WordIdx], 1u << (Slot & 31u));

    return MaterialSlotToDepth(Slot);
}
