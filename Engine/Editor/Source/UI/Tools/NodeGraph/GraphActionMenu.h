#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina
{
    class CClass;
    class CEdNodeGraph;
    class CEdGraphNode;
}

namespace Lumina
{
    // One selectable item in the action menu. Built from each CClass the graph has registered.
    struct FGraphAction
    {
        CClass*             NodeClass = nullptr;
        // Which of the class's variants this entry places, or none for the class itself.
        FName               Variant;
        FString             DisplayName;
        FString             Category;
        FString             Tooltip;

        // Filled by the fuzzy matcher; cleared when the query is empty.
        int32               Score = 0;
        TVector<uint16>     MatchIndices;

        // False when the menu opened from a dragged wire and this node has no pin that takes it.
        bool                bAcceptsSource = true;
    };

    // Incremental fuzzy search over registered node classes, with state that persists between frames until Reset.
    class FGraphActionMenu
    {
    public:

        // Draws inside an already-open popup, and the caller should CloseCurrentPopup when it returns true.
        bool Draw(CEdNodeGraph* Graph);

        // Call when the popup is opened to clear query state and refocus the input next frame.
        void Reset();

    private:

        void RebuildActions(CEdNodeGraph* Graph);
        void Rescore();
        void ClampSelection();

        char                    QueryBuffer[128] = {};
        FString                 CachedQueryForScore;
        TVector<FGraphAction>   Actions;
        TVector<int32>          VisibleIndices;
        int32                   SelectedIndex = 0;
        int32                   LastSelectedIndex = -1;
        bool                    bActionsDirty = true;
        bool                    bFocusInput = false;
        bool                    bPendingScrollToSelected = false;
        bool                    bHasSourceFilter = false;
        bool                    bOnlyAcceptingNodes = true;
    };

    // Case-insensitive subsequence match, where OutScore is valid only on a match and higher is better.
    bool FuzzyMatch(const char* Query, const char* Target, int32& OutScore, TVector<uint16>& OutMatchIndices);
}
