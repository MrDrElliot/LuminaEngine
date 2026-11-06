#pragma once

#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "imgui.h"

namespace Lumina
{
    class CClass;
    class CObject;

    // Only called for a resident asset, since loading one to draw a tile races the editor's loader.
    using FAssetTilePainterFn = TFunction<void(CObject* Asset, ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max)>;

    class FAssetTilePainterRegistry
    {
    public:

        static FAssetTilePainterRegistry& Get();

        void Register(CClass* AssetClass, FAssetTilePainterFn Painter);

        // Walks up the class hierarchy, and null means the class uses its normal thumbnail.
        FAssetTilePainterFn* Find(CClass* AssetClass);

    private:

        THashMap<CClass*, FAssetTilePainterFn> Painters;
    };

    namespace AssetTilePainters
    {
        // Also registers the matching CThumbnailManager painters, so call it after the thumbnail manager exists.
        void RegisterBuiltin();
    }
}
