#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    struct FPickerItem
    {
        FString     Name;
        // What the caller resolves the pick by, such as a full class name.
        FString     Key;
        // Slash separated, such as a script's folder, with each level becoming an expandable category.
        FString     Category;
        FString     Tooltip;
        const char* Icon = nullptr;
        // Listed but not pickable, such as a component every target already has.
        bool        bDisabled = false;
    };

    // A searchable list filed into expandable categories, where a search opens every category holding a match and preselects the best one.
    class EDITOR_API FCategorizedPicker
    {
    public:
        using FGatherItems = TFunction<void(TVector<FPickerItem>&)>;

        // A button that opens the picker in a popup, returning the picked item on the frame it is chosen.
        const FPickerItem* DrawButton(const char* StrId, const char* Label, const FGatherItems& Gather, float Width = 0.0f);

        // The picker inside a popup or menu the caller already has open, gathering each time that window appears.
        const FPickerItem* DrawContents(const FGatherItems& Gather);

        // Gathers again on the next draw while keeping the query, for a caller whose item set changed while open.
        void Invalidate() { bRegatherPending = true; }

        // Shown in the empty search box.
        FString SearchHint = "Search...";

        // The plural noun the footer counts in, such as "scripts".
        FString ItemNoun = "items";

        // A top-level category listed after the rest, such as one holding built-in entries.
        FString TrailingCategory;

    private:
        struct FCategory
        {
            FString        Label;
            TVector<int32> Children;
            TVector<int32> Items;
            int32          TotalCount = 0;
            int32          MatchCount = 0;
        };

        void Rebuild(const FGatherItems& Gather);
        int32 FindOrAddChild(int32 Parent, FStringView Label);
        void CompressChains(int32 CategoryIndex);
        int32 CountItems(int32 CategoryIndex, bool bMatchesOnly);
        void Rescore();
        void DrawCategory(int32 CategoryIndex, int32& OutPicked);
        void DrawItem(int32 ItemIndex, int32& OutPicked);
        bool IsSearching() const { return Query[0] != '\0'; }

        TVector<FPickerItem>     Items;
        // Index 0 is the root, which is never drawn itself.
        TVector<FCategory>       Categories;
        // A negative score hides the item while searching.
        TVector<int32>           ItemScores;
        TVector<TVector<uint16>> ItemMatches;
        // The items last frame drew, in order, which the arrow keys walk.
        TVector<int32>           DrawnOrder;
        TVector<int32>           DrawingOrder;
        char                     Query[128] = {};
        FString                  ScoredQuery;
        int32                    SelectedItem = -1;
        bool                     bOpenCategoriesByDefault = false;
        bool                     bScrollToSelected = false;
        bool                     bRegatherPending = false;
    };
}
