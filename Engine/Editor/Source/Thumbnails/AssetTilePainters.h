#pragma once

#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "imgui.h"

namespace Lumina
{
    class CClass;
    class CObject;

    /** Paints an asset's own content directly into a content-browser tile.
     *
     *  For assets whose value IS a shape or a swatch rather than a 3D view -- a curve, a gradient, a color
     *  -- a rendered world thumbnail is the wrong tool. It costs a scene capture, a GPU readback and a cache
     *  entry to produce a blurry 512px bitmap of something a handful of draw-list calls draw sharper, at
     *  whatever size the tile happens to be, and always in sync with the asset (no cache to invalidate when
     *  the user edits it).
     *
     *  Painters are only ever invoked with a RESIDENT asset. The content browser must never load a CObject
     *  to draw a tile -- that races the editor's own loader on a non-atomic object -- so an asset nothing
     *  has loaded yet keeps the ordinary thumbnail/fallback icon until something else loads it. Same rule
     *  CThumbnailManager::ProcessRenderQueue documents.
     *
     *  Min/Max are the tile's content rect in screen space, already inset from the button frame.
     */
    using FAssetTilePainterFn = TFunction<void(CObject* Asset, ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max)>;

    class FAssetTilePainterRegistry
    {
    public:

        static FAssetTilePainterRegistry& Get();

        void Register(CClass* AssetClass, FAssetTilePainterFn Painter);

        /** Walks up the class hierarchy, so registering on a base type covers every subclass (a painter on
         *  CMaterialInterface would serve CMaterial and CMaterialInstance alike). Null when nothing matches,
         *  which is the common case and means "use the normal thumbnail". */
        FAssetTilePainterFn* Find(CClass* AssetClass);

    private:

        THashMap<CClass*, FAssetTilePainterFn> Painters;
    };

    namespace AssetTilePainters
    {
        /** Registers the engine's built-in tile painters AND their matching CThumbnailManager thumbnail
         *  painters. Call once during editor startup, after the thumbnail manager exists. */
        void RegisterBuiltin();
    }
}
