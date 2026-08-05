#pragma once

#include "Containers/String.h"

namespace Lumina::AssetPickerFilter
{
    /** Where an asset came from, derived from the VFS root its path is mounted under. */
    enum class EAssetSource : uint8
    {
        Project,    // /Game
        Engine,     // /Engine, /Editor
        Plugin,     // every other mount alias -- plugins mount as "/<PluginName>"
    };

    /** Classifies by the leading path segment rather than by asking the plugin manager: a plugin mounts as
     *  "/<PluginName>" (FPlugin::GetMountAlias), so anything that is neither the project nor the engine is
     *  a plugin by construction -- and stays classified correctly for a plugin that failed to register. */
    EAssetSource ClassifyAssetPath(FStringView AssetPath);

    /** Which sources the asset pickers currently hide.
     *
     *  Shared by every picker rather than owned per-property: hiding engine content is a statement about
     *  what you are looking for right now, and having to re-set it on each of a dozen property slots would
     *  make it useless. Session-scoped -- it is deliberately not persisted, so a fresh editor always starts
     *  showing everything and no asset can go missing because of a setting nobody remembers changing. */
    struct FState
    {
        bool bHideEngineContent  = false;
        bool bHidePluginContent  = false;
        bool bHideProjectContent = false;

        NODISCARD bool IsAnyActive() const { return bHideEngineContent || bHidePluginContent || bHideProjectContent; }
    };

    FState& GetState();

    /** True when an asset at this path should be listed under the current filter. */
    NODISCARD bool PassesSourceFilter(FStringView AssetPath);

    /** The filter button plus its popup, drawn inline in a picker's search row. Tinted while any filter is
     *  active, so a short result list reads as "filtered" rather than as "that asset is missing". */
    void DrawFilterButton(float ButtonWidth);
}
