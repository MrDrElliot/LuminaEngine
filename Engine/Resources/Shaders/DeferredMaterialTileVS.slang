#include "Includes/SceneGlobals.slang"

// Tile-quad vertex shader for the binned deferred material pass. Each instance is one screen tile; this
// shader emits a 2-triangle quad covering the tile, but ONLY if this material's bit is set in the tile's
// bitmask (otherwise it collapses to a degenerate off-screen triangle the rasterizer drops). Replaces the
// single fullscreen triangle, so a material only rasterizes the tiles the classify pass found it in.
//
// Pairs with the per-material DeferredMaterial pixel shader; the layout below MUST match FDeferredPassArgs
// there and the C++ FDeferredPushConstants byte-for-byte (offsets 0-16 are the DBuffer overlay).

struct FDeferredPassArgs
{
    uint  VisBufferIndex;   // 0
    uint  DBufferAIndex;    // 4
    uint  DBufferBIndex;    // 8
    uint  DBufferCIndex;    // 12
    uint  MaterialIndex;    // 16  unused: superseded by SlotIndex as the per-pixel reject key
    uint  DrawListCount;    // 20
    uint  SlotIndex;        // 24  dense deferred-draw slot (bitmask bit + per-pixel reject key)
    uint  TileCountX;       // 28
    uint  TileSizePx;       // 32
    uint  TileWordCount;    // 36
    uint  ScreenW;          // 40
    uint  ScreenH;          // 44
    uint* TileBits;         // 48  per-tile slot bitmask
    uint* MaterialId;       // 56  per-pixel owning slot (classify output)
};
FDeferredPassArgs* Pass() { return GetPassArgs<FDeferredPassArgs>(); }

[shader("vertex")]
float4 VertexMain(uint VertexIndex : SV_VulkanVertexID, uint TileIndex : SV_VulkanInstanceID) : SV_Position
{
    const uint Slot = Pass().SlotIndex;
    const uint Word = Pass().TileBits[TileIndex * Pass().TileWordCount + (Slot >> 5u)];
    if (((Word >> (Slot & 31u)) & 1u) == 0u)
    {
        // This material does not cover this tile: degenerate off-screen triangle (produces no pixels).
        const float NaN = 0.0f / 0.0f;
        return float4(NaN, NaN, NaN, 1.0);
    }

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
    return float4(Ndc, 0.0, 1.0);
}
