#pragma once
#include "imgui.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/Function.h"
#include "Core/Threading/Atomic.h"

namespace Lumina
{
    class FTreeListView;

    struct RUNTIME_API FTreeNodeID
    {
        int32 Index = -1;

        FORCEINLINE bool IsValid() const { return Index >= 0; }
        FORCEINLINE bool operator==(const FTreeNodeID& Other) const { return Index == Other.Index; }
        FORCEINLINE bool operator!=(const FTreeNodeID& Other) const { return Index != Other.Index; }
    };

    inline constexpr FTreeNodeID InvalidTreeNode{};

    struct RUNTIME_API FTreeNodeDisplay
    {
        FString             DisplayName;
        FString             TooltipText;
        FString             TooltipTitle;
        FString             TooltipSubtitle;
        FString             TooltipChipHeader;
        TVector<FString>    TooltipChips;

        ImVec4          DisplayColor = ImVec4(0.725f, 0.725f, 0.725f, 1.0f);

        FString         IconText;
        ImVec4          IconColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        FString         SecondaryIconOn;
        FString         SecondaryIconOff;
        FString         SecondaryTooltip;

        uint8           bShowDisabledIcon:1 = false;
        uint8           bShowSecondaryIcon:1 = false;
		uint8		    bAllowRenaming:1 = false;
        uint8           bTooltipBuilt:1 = false;
    };

    struct RUNTIME_API FTreeNodeState
    {
        uint8 bExpanded:1       = false;
        uint8 bSelected:1       = false;
        uint8 bDisabled:1       = false;
        uint8 bEditingText:1    = false;
        uint8 bSecondaryToggled:1 = false;
    };

    struct RUNTIME_API FTreeListViewContext
    {
        /** Check if the item to draw passes a filter */
        TFunction<bool(FTreeListView&, FTreeNodeID)>                    FilterFunction;

        /** Callback to draw any context menus this item may want */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    ItemContextMenuFunction;

        /** Called when a full rebuild of the widget tree is requested */
        TFunction<void(FTreeListView&)>                                 RebuildTreeFunction;

        /** Called when an item has been selected in the tree */
        TFunction<void(FTreeListView&, FTreeNodeID, bool)>              ItemSelectedFunction;

        /** Called when an item is double-clicked in the tree */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    ItemDoubleClickedFunction;

        /** Called when we have a drag-drop operation on a target */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    DragDropFunction;

        /** Set drag drop payload */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    SetDragDropFunction;

        /** Called when a key is pressed while hovering the tile item, return true to absorb. */
        TFunction<bool(FTreeListView&, FTreeNodeID, ImGuiKey)>          KeyPressedFunction;

        /** Called when the visibility icon is toggled */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    VisibilityToggleFunction;

        /** Called when the optional secondary icon (e.g. script enable) is toggled */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    SecondaryToggleFunction;
        
        /** Called when a tree item is hovered */
        TFunction<void(FTreeListView&, FTreeNodeID)>                    HoveredFunction;

        /** Called when an item is being renamed */
        TFunction<void(FTreeListView&, FTreeNodeID, FStringView)>       RenameFunction;

        // Called once on first expand of a lazy node and must be idempotent since explicit CreateNode doesn't mark it built.
        TFunction<void(FTreeListView&, FTreeNodeID)>                    BuildChildrenFunction;

        // Shift-click selects the whole run of rows from the last plainly-clicked one to the clicked one.
        bool                                                            bAllowRangeSelect = false;

        // Cleared by an owner whose filter would pass every row, so Draw skips the per-row filter pass.
        bool                                                            bFilterActive = true;

        // Horizontal shift per tree depth, and deep hierarchies in narrow popups want it tighter.
        float                                                           IndentPerDepth = 21.0f;
    };

    // Hierarchical list over a flat node pool, and Get and EmplaceUserData refs invalidate on mutation.
    class RUNTIME_API FTreeListView
    {
    public:

        FTreeListView() = default;
        ~FTreeListView();
        LE_NO_COPYMOVE(FTreeListView);

        void Draw(const FTreeListViewContext& Context);

        // Tree mutation
        FTreeNodeID CreateNode(FTreeNodeID Parent, FStringView Name, uint64 Hash = 0);
        void RemoveNode(FTreeNodeID Handle);
        // Rebuilds each affected sibling list once, since removing one at a time scans a big folder per row.
        void RemoveNodes(const TVector<FTreeNodeID>& Handles);
        void ClearTree();

        // Dirty state
        void MarkTreeDirty() { bDirty = true; }
        NODISCARD bool IsDirty() const { return bDirty; }

        /** Force the visible-row cache to rebuild on next Draw. Call after batched mutations. */
        void InvalidateVisibleList() { bVisibleListDirty = true; }

        // Defers child creation to BuildChildrenFunction on first expand.
        void MarkHasLazyChildren(FTreeNodeID Handle, bool bHasLazy = true);

        // Expands Handle after materializing its lazy children, so a path can be walked open.
        void ExpandNode(FTreeNodeID Handle, const FTreeListViewContext& Context);

        // Scrolls to Handle next Draw and expands its ancestors, which must already be materialized (see ExpandNode).
        void RequestScrollToNode(FTreeNodeID Handle);

        NODISCARD bool IsValid(FTreeNodeID Handle) const;
        NODISCARD int32 NumNodes() const { return AliveCount; }

        // Parent node of Handle, or InvalidTreeNode if it's a root or invalid.
        NODISCARD FTreeNodeID GetParentNode(FTreeNodeID Handle) const;

        // Child enumeration, and InvalidTreeNode walks the roots.
        NODISCARD int32 NumChildNodes(FTreeNodeID Handle) const;
        NODISCARD FTreeNodeID GetChildNode(FTreeNodeID Handle, int32 ChildIndex) const;

        // Sole selection without firing ItemSelectedFunction, for reveals where the caller already applied its effects.
        void SetSelectionSilent(FTreeNodeID Handle);

        // What a click on Row selects, apart from the ImGui input that detected it.
        void ClickRow(FTreeNodeID Row, bool bShift, bool bCtrl, const FTreeListViewContext& Context);

        // Accepts FTreeNodeState, FTreeNodeDisplay, or the type installed via EmplaceUserData.
        template<typename T>
        T& Get(FTreeNodeID Handle)
        {
            FNode& Node = Nodes[Handle.Index];
            if constexpr (std::is_same_v<T, FTreeNodeState>)
            {
                return Node.State;
            }
            else if constexpr (std::is_same_v<T, FTreeNodeDisplay>)
            {
                return Node.Display;
            }
            else
            {
                return *static_cast<T*>(Node.UserData);
            }
        }

        template<typename T, typename ... TArgs>
        T& EmplaceUserData(FTreeNodeID Handle, TArgs&&... Args)
        {
            FNode& Node = Nodes[Handle.Index];
            if (Node.UserData != nullptr && Node.UserDataDeleter != nullptr)
            {
                Node.UserDataDeleter(Node.UserData);
            }
            T* NewData = new T(std::forward<TArgs>(Args)...);
            Node.UserData = NewData;
            Node.UserDataDeleter = [](void* Ptr) { delete static_cast<T*>(Ptr); };
            return *NewData;
        }

    private:

        struct FNode
        {
            int32                       ParentIdx = -1;
            TVector<int32>              Children;
            int32                       Depth = 0;
            uint64                      Hash = 0;

            FTreeNodeState              State;
            FTreeNodeDisplay            Display;

            void*                       UserData = nullptr;
            void                        (*UserDataDeleter)(void*) = nullptr;

            uint8                       bAlive:1 = false;
            uint8                       bChildrenBuilt:1 = false;
            uint8                       bHasLazyChildren:1 = false;
            uint8                       bWasExpanded:1 = false;
            uint8                       bPendingRemoval:1 = false;
        };

        void DestroyNodeData(int32 Idx);
        void RebuildTreeNow(const FTreeListViewContext& Context);
        void RebuildVisibleList();
        void AppendVisibleSubtree(int32 NodeIdx);
        void DrawSingleRow(int32 NodeIdx, const FTreeListViewContext& Context, bool& bAnyRowExpansionChanged);
        void EnsureChildrenBuilt(int32 NodeIdx, const FTreeListViewContext& Context);
        void SetSelection(FTreeNodeID Item, const FTreeListViewContext& Context, bool bShouldClear);
        // Selects Rows from FromRow to ToRow inclusive, in that order, reporting each newly selected row.
        void SelectRange(const TVector<int32>& Rows, int32 FromRow, int32 ToRow, const FTreeListViewContext& Context, bool bShouldClear);
        NODISCARD int32 FindRangeAnchorRow(const TVector<int32>& Rows, int32 TargetRow) const;
        bool HandleKeyPressed(const FTreeListViewContext& Context, FTreeNodeID Item, ImGuiKey Key);
        void ClearSelections(const FTreeListViewContext& Context);

        int32 AllocNode();
        void RemoveSubtree(int32 RootIdx);
        void UnlinkFromParent(int32 Idx);
        void RecomputeDepthsRecursive(int32 NodeIdx, int32 Depth);

    private:

        TVector<FNode>      Nodes;
        TVector<int32>      FreeList;
        TVector<int32>      Roots;

        TVector<int32>      VisibleList;     // depth-first list of currently visible row indices

        // The row list Draw is iterating; only valid inside Draw, where range selection happens.
        const TVector<int32>* CurrentRows = nullptr;

        // Node a shift-click ranges from, the last one clicked without shift, or -1.
        int32               SelectionAnchor = -1;

        // Rebuilt every frame a filter is set, since VisibleList is cached and cannot see filter text changes.
        TVector<int32>      FilteredList;
        int32               AliveCount = 0;

        // Node to scroll to next Draw, or -1, resolved once the visible list is rebuilt.
        int32               PendingScrollNode = -1;

        FString             EditLabel;

        // Bumped on every node create or destroy so Draw can stop the frame when a row callback restructures the tree.
        uint32              StructureVersion = 0;

        bool                bVisibleListDirty = true;
        bool                bDirty = false;
        TAtomic<bool>       bRebuilding = false;
    };
}
