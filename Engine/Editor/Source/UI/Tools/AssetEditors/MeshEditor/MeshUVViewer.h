#pragma once

#define USE_IMGUI_API
#include <imgui.h>

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct FMeshResource;

    /** UV inspector shared by the static and skeletal mesh editors: unwraps the mesh's UV triangles onto a
     *  pan/zoom canvas so seams, overlap, mirrored shells and out-of-tile layout are visible without leaving
     *  the editor.
     *
     *  Reads the MESHLET streams, not the import scratch. CMesh::GenerateGPUBuffers drops
     *  Positions/UVs/Indices once the GPU buffers exist, so on any loaded asset the meshlets hold the only
     *  surviving copy of the UVs -- reading FMeshResource::UVs here would show an empty unwrap for every
     *  asset that came off disk. */
    class FMeshUVViewer
    {
    public:

        /** Draws the whole window: toolbar, canvas, status line. Safe on an empty or meshlet-less resource. */
        void Draw(const FMeshResource& Resource);

        /** Drops the cached edge list; the next Draw rebuilds it. Call after the mesh data changes in a way
         *  the cache key cannot see (a reimport that happens to keep the same counts). */
        void Invalidate();

    private:

        /** One UV-space line segment, pre-unpacked at build time so drawing is a transform and nothing else. */
        struct FUVEdge
        {
            ImVec2 A;
            ImVec2 B;
            uint16 Surface  = 0;
            bool   bOutside = false;    // either endpoint leaves the 0..1 tile
        };

        /** Identifies the data the cached edges were built from. Counts are part of it so a reimport that
         *  changes the mesh rebuilds without any explicit notification. */
        struct FCacheKey
        {
            const FMeshResource* Resource     = nullptr;
            uint32               LOD          = 0;
            uint32               MeshletCount = 0;
            uint32               VertexCount  = 0;

            bool operator==(const FCacheKey& Other) const = default;
        };

        void RebuildEdges(const FMeshResource& Resource);
        void DrawToolbar(const FMeshResource& Resource);
        void DrawCanvas(const FMeshResource& Resource);

        /** Frames the 0..1 tile in the canvas. */
        void FitView(const ImVec2& CanvasSize);

        FORCEINLINE ImVec2 ToScreen(const ImVec2& UV) const
        {
            return ImVec2(CanvasOrigin.x + UV.x * Zoom + Pan.x, CanvasOrigin.y + UV.y * Zoom + Pan.y);
        }

        FORCEINLINE ImVec2 ToUV(const ImVec2& Screen) const
        {
            return ImVec2((Screen.x - CanvasOrigin.x - Pan.x) / Zoom, (Screen.y - CanvasOrigin.y - Pan.y) / Zoom);
        }

        TVector<FUVEdge> Edges;
        FCacheKey        CacheKey;

        // Not part of FCacheKey because it selects which edges get collected rather than describing the
        // source data; kept alongside so a filter change rebuilds the same way a LOD change does.
        int32            CachedSurfaceFilter = -2;   // -2 = never built (-1 is a valid filter: all surfaces)

        uint32 TriangleCount        = 0;    // triangles walked for the current LOD/cache
        uint32 OutsideTriangleCount = 0;    // of those, ones that leave the 0..1 tile
        bool   bTruncated           = false; // hit MaxEdges and stopped collecting

        int32 LODIndex      = 0;
        int32 SurfaceFilter = -1;   // -1 = all surfaces

        bool bShowGrid      = true;
        bool bShowTiles     = true;
        bool bShowVertices  = false;
        bool bHighlightOverflow = true;

        // Pixels per UV unit, and the canvas-space offset of UV (0,0).
        float  Zoom         = 0.0f;     // <= 0 means "fit on the next draw"
        ImVec2 Pan          = ImVec2(0.0f, 0.0f);
        ImVec2 CanvasOrigin = ImVec2(0.0f, 0.0f);

        /** Ceiling on collected segments. A dense mesh would otherwise push millions of lines through
         *  ImDrawList every frame; the LOD picker is the escape hatch, and the status line says so. */
        static constexpr uint32 MaxEdges = 120000;
    };
}
