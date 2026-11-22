#pragma once
#include "imgui.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Memory/Memory.h"
#include "Memory/Allocators/Allocator.h"

namespace Lumina
{

    class RUNTIME_API FTileViewItem
    {
        friend class FTileViewWidget;
    public:
        
        enum class EClickState : uint8
        {
            None,
            Single,
            SingleWithCtrl,
            SingleWithShift,
            Double,
        };
        

        FTileViewItem(FTileViewItem* InParent)
            : bExpanded(false)
            , bVisible(false)
            , bSelected(false)
            , bDisplayNameCached(false)
        {}

        virtual ~FTileViewItem() = default;
        LE_NO_COPYMOVE(FTileViewItem);

        virtual FStringView GetName() const { return {}; }

        /** Short, dimmed line under the name -- the item's TYPE. Empty draws nothing, but the band is
         *  still reserved when the context asks for it, so every row keeps the same height. */
        virtual FStringView GetTypeLabel() const { return {}; }
        
        virtual void DrawTooltip() const { }

        virtual bool HasContextMenu() { return false; }

        virtual ImVec4 GetDisplayColor() const { return {};  }

        virtual void OnSelectionStateChanged() { }

        virtual void SetDragDropPayloadData() const { }
        
        virtual FFixedString GetDisplayName() const
        {
            return { GetName().begin(), GetName().end() };
        }

        // Resolved once on first draw because GetName and GetDisplayName may parse paths and allocate.
        FStringView GetCachedDisplayName()
        {
            if (!bDisplayNameCached)
            {
                const FFixedString Resolved = GetDisplayName();
                CachedDisplayName = FStringView(*Resolved, Resolved.Len());
                bDisplayNameCached = true;
            }
            return FStringView(CachedDisplayName.GetData(), CachedDisplayName.Len());
        }

        bool IsSelected() const { return bSelected; }

    protected:

        // Heap string because a fixed 255-char buffer would be paid by every item.
        FString                     CachedDisplayName;

        uint8                       bExpanded:1;
        uint8                       bVisible:1;
        uint8                       bSelected:1;
        uint8                       bDisplayNameCached:1;

    };

    class FTileViewWidget;

    struct RUNTIME_API FTileViewContext
    {
        /** Callback to draw any context menus this item may want */
        TFunction<void(const TVector<FTileViewItem*>&)>         DrawItemContextMenuFunction;

        /** Callback to override item drawing */
        TFunction<FTileViewItem::EClickState(FTileViewItem*)>  DrawItemOverrideFunction;

        /** Called when a rebuild of the widget tree is requested */
        TFunction<void(FTileViewWidget*)>                       RebuildTreeFunction;

        /** Called when an item has been selected in the tree */
        TFunction<void(FTileViewItem*)>                         ItemSelectedFunction;
        
        /** Called when an item has been double-clicked. */
        TFunction<void(FTileViewItem*)>                         ItemDoubleClickedFunction;

        /** Called when we have a drag-drop operation on a target */
        TFunction<void(FTileViewItem*, const TVector<FTileViewItem*>&)>         DragDropFunction;

        /** Called when a key is pressed while hovering the tile item, return true to absorb. */
        TFunction<bool(FTileViewItem&, ImGuiKey)>               KeyPressedFunction;

        /** Called when an inline rename is committed with the new name. */
        TFunction<void(FTileViewItem*, const char*)>            ItemRenamedFunction;

        /** Reserve a second label line for FTileViewItem::GetTypeLabel under every name. */
        bool                                                    bShowTypeLabels = false;

    };

    class RUNTIME_API FTileViewWidget
    {
    public:
        
        FTileViewWidget() = default;
        ~FTileViewWidget() { ClearTree(); }
        LE_NO_COPYMOVE(FTileViewWidget);

        void Draw(const FTileViewContext& Context);
        void ClearTree();
        void MarkTreeDirty() { bDirty = true; }

        FORCEINLINE bool IsDirty() const { return bDirty; }
        
        template<typename T, typename... Args>
        requires (std::is_base_of_v<FTileViewItem, T> && std::is_constructible_v<T, Args...>)
        T* AddItemToTree(Args&&... args);

        void ClearSelections();

        // Makes Item the sole selection and scrolls to it next Draw. Safe inside a rebuild since the target is stored as an index.
        void SelectAndScrollTo(FTileViewItem* Item);
        
        const TVector<FTileViewItem*>& GetSelections() const { return Selections; }

		float GetTileSize() const { return TileSize; }
		void SetTileSize(float Size) { TileSize = Size; }

        /** Turns the item's label into an in-place text field, focused with the text selected. */
        void BeginInlineRename(FTileViewItem* Item);
        void CancelInlineRename();

        FORCEINLINE bool IsRenaming() const { return RenamingItem != nullptr; }

        // True while a rubber-band drag is in progress, so callers can suppress renames and context menus.
        FORCEINLINE bool IsMarqueeActive() const { return bMarqueeActive; }

    private:

        void CommitInlineRename(const FTileViewContext& Context);

        // Keeps the normal label band height so the clipper's row height never changes mid-edit.
        void DrawInlineRename(const FTileViewContext& Context);

        bool HandleKeyPressed(const FTileViewContext& Context, FTileViewItem& Item, ImGuiKey Key);

        /** Ctrl+A. Covers every item in the folder, including rows the clipper never submitted. */
        void SelectAll(const FTileViewContext& Context);

        // Runs per frame against the selection, since keying off the hovered tile loses presses after a rename re-sorts the tree.
        void HandleSelectionKeys(const FTileViewContext& Context);

        void RebuildTree(const FTileViewContext& Context);

        // Draws one full cell (icon button + label) as a single group for SameLine layout.
        void DrawTile(FTileViewItem* Item, const FTileViewContext& Context);

        void DrawItem(FTileViewItem* ItemToDraw, const FTileViewContext& Context, ImVec2 DrawSize);

        void ToggleSelection(FTileViewItem* Item, const FTileViewContext& Context);

        void SetSelectionAnchor(const FTileViewItem* Item);

        // Shift-click selects the anchor through Item inclusive, and ListItems is in draw order so the index range is the visual range.
        void SelectRangeTo(FTileViewItem* Item, const FTileViewContext& Context);

        // Rubber-band selection. Begin and end run at the end of Draw so IsAnyItemHovered can tell empty space from a tile.
        void UpdateMarquee(const FTileViewContext& Context);

        // Screen-space rect for this frame, or false when no drag is active.
        bool GetMarqueeScreenRect(ImVec2& OutMin, ImVec2& OutMax) const;
    

    private:

        // A content-browser tile is about 1KB on its own, so the 1024-byte default gave one heap block per tile.
        FBlockLinearAllocator                   Allocator{ 64 * 1024 };

        TVector<FTileViewItem*>                 Selections;

        /** Root nodes */
        TVector<FTileViewItem*>                 ListItems;

        /** Item currently being renamed in place. Owned by Allocator, so ClearTree must null it. */
        FTileViewItem*                          RenamingItem = nullptr;

        bool                                    bMarqueeActive = false;

        // Content space rather than screen space, because the view auto-scrolls during a drag.
        ImVec2                                  MarqueeAnchorContent = ImVec2(0.0f, 0.0f);

        // Selection at drag start, so shrinking an additive band keeps what was already selected.
        TVector<FTileViewItem*>                 MarqueeBaseSelection;
        bool                                    bMarqueeAdditive = false;

        /** Index into ListItems to select and scroll to on the next Draw (-1 = none). */
        int32                                   PendingRevealIndex = -1;

        // Origin a shift-click ranges from. An index, so a rebuild has to clear it.
        int32                                   SelectionAnchorIndex = -1;

        FString                                 RenameText;

		float                                   TileSize = 84.0f;

        uint8                                   bDirty:1 = false;
        uint8                                   bRenameFocusPending:1 = false;
        uint8                                   bRenameWasActive:1 = false;
    };


    
    template <typename T, typename ... Args>
    requires (std::is_base_of_v<FTileViewItem, T> && std::is_constructible_v<T, Args...>)
    T* FTileViewWidget::AddItemToTree(Args&&... args)
    {
        T* New = Allocator.TAlloc<T>(std::forward<Args>(args)...);
        ListItems.Add(New);
        return New;
    }
}
