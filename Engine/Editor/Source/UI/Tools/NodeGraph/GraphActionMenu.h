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
        FString             DisplayName;
        FString             Category;
        FString             Tooltip;

        // Filled by the fuzzy matcher; cleared when the query is empty.
        int32               Score = 0;
        TVector<uint16>     MatchIndices;
    };

    // Incremental fuzzy search over the graph's registered node classes (keyboard nav, highlighting,
    // relevance order). State persists between frames; call Reset() when the popup opens.
    class FGraphActionMenu
    {
    public:

        // Draws the action menu inside an already-open ImGui popup. Returns true if an action was
        // committed this frame, caller should CloseCurrentPopup in that case.
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
    };

    // Case-insensitive fuzzy subsequence match; OutScore valid only on match (higher is better),
    // OutMatchIndices receives the matched Target positions for highlighting.
    bool FuzzyMatch(const char* Query, const char* Target, int32& OutScore, TVector<uint16>& OutMatchIndices);
}
