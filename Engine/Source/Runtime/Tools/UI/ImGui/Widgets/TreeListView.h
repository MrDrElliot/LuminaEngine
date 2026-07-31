#pragma once
#include "imgui.h"
#include "Containers/Array.h"
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

        // Called once when a lazy-children node is first expanded; add children via CreateNode().
        // Must be idempotent (skip already-added children); explicit CreateNode doesn't mark built.
        TFunction<void(FTreeListView&, FTreeNodeID)>                    BuildChildrenFunction;

        // Horizontal shift per tree depth. Deep hierarchies in narrow hosts (picker popups over
        // 20+-level skeletons) want a tighter value than the outliner default.
        float                                                           IndentPerDepth = 21.0f;
    };

    // Hierarchical list over a flat node pool for very large trees; Draw iterates a cached visible-row
    // list so the clipper skips off-screen work. Get<>/EmplaceUserData<> refs invalidate on mutation.
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
        void ClearTree();

        // Dirty state
        void MarkTreeDirty() { bDirty = true; }
        NODISCARD bool IsDirty() const { return bDirty; }

        /** Force the visible-row cache to rebuild on next Draw. Call after batched mutations. */
        void InvalidateVisibleList() { bVisibleListDirty = true; }

        // Flag a node's children to be created on demand; BuildChildrenFunction fires the first time
        // it's expanded. Eager CreateNode on the parent beforehand still works.
        void MarkHasLazyChildren(FTreeNodeID Handle, bool bHasLazy = true);

        // Expands Handle, materializing its lazy children through Context first so descendants have
        // rows. Use to walk a path open before revealing something below a collapsed node.
        void ExpandNode(FTreeNodeID Handle, const FTreeListViewContext& Context);

        // Scrolls Handle's row into view on the next Draw, expanding its ancestors so the row is
        // actually in the visible list. Ancestors must already be materialized (see ExpandNode).
        void RequestScrollToNode(FTreeNodeID Handle);

        NODISCARD bool IsValid(FTreeNodeID Handle) const;
        NODISCARD int32 NumNodes() const { return AliveCount; }

        // Parent node of Handle, or InvalidTreeNode if it's a root or invalid.
        NODISCARD FTreeNodeID GetParentNode(FTreeNodeID Handle) const;

        // Child enumeration; pass InvalidTreeNode to walk the root nodes. Lets callers resolve a node
        // by walking a path down from a root, expanding as they go.
        NODISCARD int32 NumChildNodes(FTreeNodeID Handle) const;
        NODISCARD FTreeNodeID GetChildNode(FTreeNodeID Handle, int32 ChildIndex) const;

        // Marks Handle as the sole selection without firing ItemSelectedFunction; for programmatic
        // reveals where the caller has already applied whatever the selection would have done.
        void SetSelectionSilent(FTreeNodeID Handle);

        // Get<FTreeNodeState>, Get<FTreeNodeDisplay>, or Get<UserDataT> (the type previously
        // installed via EmplaceUserData on this node).
        template<typename T>
        T& Get(FTreeNodeID Handle)
        {
            FNode& Node = Nodes[Handle.Index];
            if constexpr (eastl::is_same_v<T, FTreeNodeState>)
            {
                return Node.State;
            }
            else if constexpr (eastl::is_same_v<T, FTreeNodeDisplay>)
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
            T* NewData = new T(eastl::forward<TArgs>(Args)...);
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
        };

        void DestroyNodeData(int32 Idx);
        void RebuildTreeNow(const FTreeListViewContext& Context);
        void RebuildVisibleList();
        void AppendVisibleSubtree(int32 NodeIdx);
        void DrawSingleRow(int32 NodeIdx, const FTreeListViewContext& Context, bool& bAnyRowExpansionChanged);
        void EnsureChildrenBuilt(int32 NodeIdx, const FTreeListViewContext& Context);
        void SetSelection(FTreeNodeID Item, const FTreeListViewContext& Context, bool bShouldClear);
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
        int32               AliveCount = 0;

        // Node to scroll into view on the next Draw (-1 = none); resolved to a row once the
        // visible list is rebuilt, then cleared.
        int32               PendingScrollNode = -1;

        bool                bVisibleListDirty = true;
        bool                bDirty = false;
        TAtomic<bool>       bRebuilding = false;
    };
}
