#include "Includes/SceneGlobals.slang"
#include "Includes/MaterialTiles.slang"

// Tile-quad vertex shader for the binned deferred material pass. Each instance is one tile this material
// actually covers, read straight out of the compacted list BuildMaterialTileList produced -- so there is no
// self-culling and no degenerate output. The draw is indirect and its instance count IS the material's tile
// count, so a material that covers 30 tiles launches 30 instances, not one per tile on screen.
//
// The quad is emitted at the material's encoded slot depth. Paired with a DEPTH_EQUAL test against the
// MaterialDepth target, that is what rejects pixels owned by other materials -- in fixed-function hardware,
// before any pixel shader launches, at coarse depth-block granularity. The pixel shader has no reject of
// its own and never discards.
//
// The layout below MUST match FDeferredPassArgs in DeferredMaterial.slang and the C++
// FDeferredPushConstants byte-for-byte (offsets 0-12 are the DBuffer overlay).

struct FDeferredPassArgs
{
    uint  VisBufferIndex;   // 0
    uint  DBufferAIndex;    // 4
    uint  DBufferBIndex;    // 8
    uint  DBufferCIndex;    // 12
    uint  DrawListCount;    // 16
    uint  SlotIndex;        // 20  dense deferred-draw slot; encodes this draw's depth
    uint  TileCountX;       // 24
    uint  TileSizePx;       // 28
    uint  ScreenW;          // 32
    uint  ScreenH;          // 36
    uint* TileList;         // 40  compacted per-slot tile list (Slot * TotalTiles strided)
};
FDeferredPassArgs* Pass() { return GetPassArgs<FDeferredPassArgs>(); }

[shader("vertex")]
float4 VertexMain(uint VertexIndex : SV_VulkanVertexID, uint ListIndex : SV_VulkanInstanceID) : SV_Position
{
    // SV_VulkanInstanceID includes the draw's FirstInstance, which the args carry as Slot * TotalTiles, so
    // this indexes the global list directly with no per-slot offset math here.
    const uint TileIndex = Pass().TileList[ListIndex];

    const uint TileX = TileIndex % Pass().TileCountX;
    const uint TileY = TileIndex / Pass().TileCountX;

    const uint TS    = Pass().TileSizePx;
    const float MinX = (float)(TileX * TS);
    const float MinY = (float)(TileY * TS);
    const float MaxX = (float)min((TileX + 1u) * TS, Pass().ScreenW);
    const float MaxY = (float)min((TileY + 1u) * TS, Pass().ScreenH);

    // Two triangles, 6 corners of the tile's pixel rect: (0,0)(1,0)(0,1) + (1,0)(1,1)(0,1).
    const uint2 Corners[6] = {
        uint2(0u, 0u), uint2(1u, 0u), uint2(0u, 1u),
        uint2(1u, 0u), uint2(1u, 1u), uint2(0u, 1u),
    };
    const uint2 C = Corners[VertexIndex];

    const float Px = (C.x == 0u) ? MinX : MaxX;
    const float Py = (C.y == 0u) ? MinY : MaxY;

    // Pixel rect -> NDC, same linear map FullscreenQuad uses (fb = (ndc+1)/2 * size), so the pixel shader's
    // int2(SV_Position.xy) and PixelNdc land on exactly these texels.
    float2 Ndc;
    Ndc.x = (Px / (float)Pass().ScreenW) * 2.0 - 1.0;
    Ndc.y = (Py / (float)Pass().ScreenH) * 2.0 - 1.0;

    // w = 1 so the encoded depth survives the perspective divide unchanged and reaches the depth test as the
    // exact value MaterialDepth stamped. Both sides call MaterialSlotToDepth on the same integer, so the
    // DEPTH_EQUAL comparison is bit-exact rather than approximately equal.
    return float4(Ndc, MaterialSlotToDepth(Pass().SlotIndex), 1.0);
}
