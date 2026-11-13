#pragma once

#include "Assets/AssetManager/AssetManager.h"
#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "imgui.h"

namespace Lumina
{
    class CClass;
    class CObject;
    struct FAssetData;

    // Only called for a resident asset, since loading one to draw a tile races the editor's loader.
    using FAssetTilePainterFn = TFunction<void(CObject* Asset, ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max)>;

    // What a tile action started, polled until it ends and stopped early by a second click.
    struct FAssetTilePreview
    {
        TFunction<bool()> IsRunning;
        TFunction<void()> Stop;
    };

    // A button drawn over a hovered tile, such as play on a sound.
    struct FAssetTileAction
    {
        const char* Icon       = nullptr;
        const char* ActiveIcon = nullptr;

        // Only called with the asset resident, and an empty preview means nothing started.
        TFunction<FAssetTilePreview(CObject* Asset)> Start;
    };

    // The one preview tile actions run at a time, owned by the tool drawing the tiles and stopped when that tool goes.
    class EDITOR_API FAssetTilePreviewSlot
    {
    public:

        ~FAssetTilePreviewSlot() { Stop(); }

        // Replaces whatever ran before, and starts once Tick sees the asset loaded.
        void Request(const FAssetTileAction& InAction, const FAssetData& InAsset);
        void Stop();

        // Called every frame the tiles draw, since a hidden drawer or background tab still ticks.
        void MarkShown() { bShown = true; }

        // Also stops a preview that ended, whose asset was deleted or renamed, or whose tiles went undrawn since the last Tick.
        void Tick();

        NODISCARD bool IsActiveFor(const FGuid& InAsset) const { return Asset.IsValid() && Asset == InAsset; }

    private:

        void Advance();

        FAssetTileAction  Action;
        FAssetHandle      Load;
        FGuid             Asset;
        FFixedString      AssetPath;
        FAssetTilePreview Preview;
        bool              bShown = false;
    };

    class EDITOR_API FAssetTilePainterRegistry
    {
    public:

        static FAssetTilePainterRegistry& Get();

        void Register(CClass* AssetClass, FAssetTilePainterFn Painter);
        void RegisterAction(CClass* AssetClass, FAssetTileAction Action);

        // Walks up the class hierarchy, and null means the class uses its normal thumbnail.
        FAssetTilePainterFn* Find(CClass* AssetClass);

        // Walks up the class hierarchy like Find.
        const FAssetTileAction* FindAction(CClass* AssetClass);

    private:

        THashMap<CClass*, FAssetTilePainterFn> Painters;
        THashMap<CClass*, FAssetTileAction>    Actions;
    };

    namespace AssetTilePainters
    {
        // Also registers the matching CThumbnailManager painters, so call it after the thumbnail manager exists.
        void RegisterBuiltin();
    }
}
