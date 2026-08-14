#pragma once
#include "imgui.h"
#include "Containers/Array.h"
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

        // Display name resolved once on first draw, then reused. GetName()/GetDisplayName() may
        // parse paths and allocate, so they must never run per-frame in the draw loop.
        FStringView GetCachedDisplayName()
        {
            if (!bDisplayNameCached)
            {
                const FFixedString Resolved = GetDisplayName();
                CachedDisplayName.assign(Resolved.c_str(), Resolved.size());
                bDisplayNameCached = true;
            }
            return FStringView(CachedDisplayName.data(), CachedDisplayName.size());
        }

        bool IsSelected() const { return bSelected; }

    protected:

        // Heap string, not a fixed one: a 255-char inline buffer here is paid by EVERY item, and a
        // display name is a leaf filename that fits the small-string buffer most of the time.
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
        ~FTileViewWidget() = default;
        LE_NO_COPYMOVE(FTileViewWidget);

        void Draw(const FTileViewContext& Context);
        void ClearTree();
        void MarkTreeDirty() { bDirty = true; }

        FORCEINLINE bool IsDirty() const { return bDirty; }
        
        template<typename T, typename... Args>
        requires (eastl::is_base_of_v<FTileViewItem, T> && eastl::is_constructible_v<T, Args...>)
        T* AddItemToTree(Args&&... args);

        void ClearSelections();

        // Makes Item the sole selection and scrolls it into view on the next Draw. Safe to call from
        // inside a rebuild: the target is stored as an index, so ClearTree cannot leave it dangling.
        void SelectAndScrollTo(FTileViewItem* Item);
        
        const TVector<FTileViewItem*>& GetSelections() const { return Selections; }

		float GetTileSize() const { return TileSize; }
		void SetTileSize(float Size) { TileSize = Size; }

        /** Turns the item's label into an in-place text field, focused with the text selected. */
        void BeginInlineRename(FTileViewItem* Item);
        void CancelInlineRename();

        FORCEINLINE bool IsRenaming() const { return RenamingItem != nullptr; }

        // True while a rubber-band drag is in progress. Callers use it to suppress anything that would
        // fight the drag (starting a rename, opening a context menu).
        FORCEINLINE bool IsMarqueeActive() const { return bMarqueeActive; }

    private:

        void CommitInlineRename(const FTileViewContext& Context);

        // Draws the label band as an in-place text field. Returns the same height a normal label
        // band occupies so the clipper's row height never changes mid-edit.
        void DrawInlineRename(const FTileViewContext& Context);

        bool HandleKeyPressed(const FTileViewContext& Context, FTileViewItem& Item, ImGuiKey Key);

        /** Ctrl+A. Covers every item in the folder, including rows the clipper never submitted. */
        void SelectAll(const FTileViewContext& Context);

        // Runs once per frame against the selection. Keying off the hovered tile instead loses presses:
        // a rename re-sorts the tree, the tile slides out from under the cursor, and the next press
        // lands on empty space.
        void HandleSelectionKeys(const FTileViewContext& Context);

        void RebuildTree(const FTileViewContext& Context, bool bKeepSelections = false);

        // Draws one full cell (icon button + label) as a single group for SameLine layout.
        void DrawTile(FTileViewItem* Item, const FTileViewContext& Context);

        void DrawItem(FTileViewItem* ItemToDraw, const FTileViewContext& Context, ImVec2 DrawSize);

        void ToggleSelection(FTileViewItem* Item, const FTileViewContext& Context);

        void SetSelectionAnchor(const FTileViewItem* Item);

        // Shift-click: selects everything between the anchor and Item inclusive. ListItems is in draw
        // order, so an index range is the visual range.
        void SelectRangeTo(FTileViewItem* Item, const FTileViewContext& Context);

        // Rubber-band selection. Begin/end detection runs at the END of Draw, once every tile has been
        // submitted and IsAnyItemHovered() can tell "empty space" from "on a tile"; the RECT is rebuilt at
        // the top of the next Draw so the tiles hit-test against the live mouse position, not a stale one.
        void UpdateMarquee(const FTileViewContext& Context);

        // Screen-space rect for this frame, or false when no drag is active.
        bool GetMarqueeScreenRect(ImVec2& OutMin, ImVec2& OutMax) const;
    

    private:

        // Sized off the items rather than left at the 1024-byte default: a content-browser tile carries
        // a VFS::FFileInfo whose fixed strings make it ~1KB on its own, so the default gave one heap
        // block per tile -- the allocator's whole point is batching, and it was doing none.
        FBlockLinearAllocator                   Allocator{ 64 * 1024 };

        TVector<FTileViewItem*>                 Selections;

        /** Root nodes */
        TVector<FTileViewItem*>                 ListItems;

        /** Item currently being renamed in place. Owned by Allocator, so ClearTree must null it. */
        FTileViewItem*                          RenamingItem = nullptr;

        bool                                    bMarqueeActive = false;

        // Anchor is stored in CONTENT space, not screen space: the view scrolls while you drag (holding
        // the mouse near an edge auto-scrolls), and a screen-space anchor would slide with it.
        ImVec2                                  MarqueeAnchorContent = ImVec2(0.0f, 0.0f);

        // Selection at drag start, restored-into on an additive (Ctrl) drag so shrinking the band gives
        // back what the drag added without dropping what was already selected.
        TVector<FTileViewItem*>                 MarqueeBaseSelection;
        bool                                    bMarqueeAdditive = false;

        /** Index into ListItems to select and scroll to on the next Draw (-1 = none). */
        int32                                   PendingRevealIndex = -1;

        // Origin a shift-click ranges from. An index, so a rebuild has to clear it.
        int32                                   SelectionAnchorIndex = -1;

        char                                    RenameBuffer[128] = {};

		float                                   TileSize = 84.0f;

        uint8                                   bDirty:1 = false;
        uint8                                   bRenameFocusPending:1 = false;
        uint8                                   bRenameWasActive:1 = false;
    };


    
    template <typename T, typename ... Args>
    requires (eastl::is_base_of_v<FTileViewItem, T> && eastl::is_constructible_v<T, Args...>)
    T* FTileViewWidget::AddItemToTree(Args&&... args)
    {
        T* New = Allocator.TAlloc<T>(eastl::forward<Args>(args)...);
        ListItems.push_back(New);
        return New;
    }
}
