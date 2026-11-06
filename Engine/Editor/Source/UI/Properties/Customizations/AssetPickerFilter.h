#pragma once

#include "Containers/String.h"
#include "imgui.h"

namespace Lumina
{
    class CClass;
    struct FAssetData;
}

namespace Lumina::AssetPickerFilter
{
    // One definition for every picker row, so object, id and document slots match and unity builds see no duplicates.
    constexpr ImVec2 kButtonSize(42, 0);
    constexpr ImVec4 kMutedText(0.6f, 0.6f, 0.6f, 1.0f);
    constexpr ImVec4 kEmptyText(1.0f, 0.19f, 0.19f, 1.0f);

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

    // The Content Browser selection when its class is-a Class, else null.
    NODISCARD const FAssetData* GetSelectedAssetOfClass(const CClass* Class);

    // Disabled with Hint as its tooltip until the Content Browser selection fits, true on click.
    bool DrawUseSelectedButton(bool bSelectionFits, const char* Hint);

    // Reveals AssetPath in the Content Browser, disabled while the path is empty.
    void DrawBrowseButton(FStringView AssetPath);

    bool DrawClearButton(const char* Tooltip);
}
