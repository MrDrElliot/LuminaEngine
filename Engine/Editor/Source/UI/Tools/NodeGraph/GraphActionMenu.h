#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "UI/Widgets/CategorizedPicker.h"

namespace Lumina
{
    class CClass;
    class CEdNodeGraph;
}

namespace Lumina
{
    // The node a picker entry places, which its key indexes.
    struct FGraphAction
    {
        CClass* NodeClass = nullptr;
        // Which of the class's variants this entry places, or none for the class itself.
        FName   Variant;
    };

    // The add-node menu, the editor's categorized picker over every node class the graph registers.
    class FGraphActionMenu
    {
    public:

        FGraphActionMenu();

        // Draws inside an already-open popup, and the caller should CloseCurrentPopup when it returns true.
        bool Draw(CEdNodeGraph* Graph);

        // Call when the popup is opened, so a menu opened from a dragged wire starts filtered to the nodes that take it.
        void Reset();

    private:

        void GatherItems(CEdNodeGraph* Graph, TVector<FPickerItem>& OutItems);

        FCategorizedPicker      Picker;
        TVector<FGraphAction>   Actions;
        bool                    bHasSourceFilter = false;
        bool                    bOnlyAcceptingNodes = true;
    };

    // Case-insensitive subsequence match, where OutScore is valid only on a match and higher is better.
    bool FuzzyMatch(const char* Query, const char* Target, int32& OutScore, TVector<uint16>& OutMatchIndices);
}
