#include "RuntimePCH.h"
#include "TileViewWidget.h"

#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace
    {
        constexpr float GTileSpacing  = 5.0f;   // gap between cells, horizontal and vertical
        constexpr float GLabelGap     = 4.0f;   // gap between the icon button and its label
        constexpr float GLabelHeight  = 36.0f;  // fixed label band; keeps every row the same height
        constexpr float GTypeLabelPad = 3.0f;   // breathing room under the type line; band = font + this
        constexpr float GColumnBudget = 8.0f;   // slack for the button's frame padding when packing columns
    }

    void FTileViewWidget::Draw(const FTileViewContext& Context)
    {
        // Rebuild lazily, then fall through and draw the fresh tree the same frame (no blank frame).
        if (bDirty)
        {
            RebuildTree(Context);
        }

        // Apply a pending reveal now the tree is final. Consumed either way, so a target that no
        // longer exists (the folder changed under us) simply lapses.
        int32 RevealIndex = PendingRevealIndex;
        PendingRevealIndex = -1;
        if (RevealIndex >= 0 && RevealIndex < (int32)ListItems.size())
        {
            ClearSelections();
            ToggleSelection(ListItems[RevealIndex], Context);
        }
        else
        {
            RevealIndex = -1;
        }

        const int ItemCount = (int)ListItems.size();
        if (ItemCount == 0)
        {
            return;
        }

        const float PaneWidth   = ImGui::GetContentRegionAvail().x;
        const float CellWidth   = TileSize + GColumnBudget + GTileSpacing;
        const int   ItemsPerRow = std::max(1, (int)((PaneWidth + GTileSpacing) / CellWidth));
        const int   RowCount    = (ItemCount + ItemsPerRow - 1) / ItemsPerRow;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(GTileSpacing, GTileSpacing));

        const int ScrollToRow = (RevealIndex >= 0) ? (RevealIndex / ItemsPerRow) : -1;

        // Virtualize by row: the clipper measures one row's height and only submits visible rows,
        // so a folder with thousands of files costs the same as one screenful.
        ImGuiListClipper Clipper;
        Clipper.Begin(RowCount);

        // The revealed row is almost always off-screen (that's the point), so force the clipper to
        // submit it -- SetScrollHereY needs the row's real item rect.
        if (ScrollToRow != -1)
        {
            Clipper.IncludeItemByIndex(ScrollToRow);
        }

        while (Clipper.Step())
        {
            for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
            {
                const int RowBegin = Row * ItemsPerRow;
                const int RowEnd   = std::min(RowBegin + ItemsPerRow, ItemCount);

                for (int Index = RowBegin; Index < RowEnd; ++Index)
                {
                    if (Index > RowBegin)
                    {
                        ImGui::SameLine();
                    }
                    DrawTile(ListItems[Index], Context);
                }

                if (Row == ScrollToRow)
                {
                    ImGui::SetScrollHereY(0.5f);
                }
            }
        }
        Clipper.End();

        ImGui::PopStyleVar();

        // After every tile: the start test needs IsAnyItemHovered() to be meaningful, and the band should
        // paint over the tiles rather than under them.
        UpdateMarquee(Context);

        HandleSelectionKeys(Context);
    }

    void FTileViewWidget::DrawTile(FTileViewItem* Item, const FTileViewContext& Context)
    {
        ImGui::PushID(Item);
        ImGui::BeginGroup();

        DrawItem(Item, Context, ImVec2(TileSize, TileSize));

        if (Item == RenamingItem)
        {
            ImGui::Dummy(ImVec2(0.0f, GLabelGap));
            DrawInlineRename(Context);

            ImGui::EndGroup();
            ImGui::PopID();
            return;
        }

        // Draw the label as a raw draw-list primitive (not an ImGui item) so the cell's logical
        // height stays fixed regardless of name length, keeping the row clipper aligned.
        const FStringView Name = Item->GetCachedDisplayName();
        const char* NameBegin  = Name.data();
        const char* NameEnd    = Name.data() + Name.size();

        // Long names get an ellipsis rather than being sliced mid-glyph by the clip rect below. The
        // buffer must outlive the AddText call, so it lives out here.
        char Truncated[256];

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
        ImFont*      LabelFont = ImGui::GetFont();
        const float  FontSize  = ImGui::GetFontSize();
        ImVec2       TextSize  = ImGui::CalcTextSize(NameBegin, NameEnd, false, TileSize);

        if (TextSize.y > GLabelHeight)
        {
            static constexpr char Ellipsis[]  = "...";
            constexpr size_t      EllipsisLen = sizeof(Ellipsis) - 1;

            // Longest prefix that still fits the band once the ellipsis is appended. Bisected rather
            // than walked, and measured through CalcTextSize so ImGui's own wrapping decides the fit --
            // reproducing its line breaks by hand is what gets this subtly wrong. Only names that
            // actually overflow pay for the handful of extra measurements.
            size_t Low  = 0;
            size_t High = std::min(Name.size(), sizeof(Truncated) - EllipsisLen - 1);
            while (Low < High)
            {
                const size_t Mid = (Low + High + 1) / 2;
                memcpy(Truncated, NameBegin, Mid);
                memcpy(Truncated + Mid, Ellipsis, EllipsisLen);

                if (ImGui::CalcTextSize(Truncated, Truncated + Mid + EllipsisLen, false, TileSize).y <= GLabelHeight)
                {
                    Low = Mid;
                }
                else
                {
                    High = Mid - 1;
                }
            }

            // Never cut inside a multi-byte sequence; a split UTF-8 codepoint renders as a broken glyph.
            while (Low > 0 && ((uint8)NameBegin[Low] & 0xC0) == 0x80)
            {
                --Low;
            }

            memcpy(Truncated, NameBegin, Low);
            memcpy(Truncated + Low, Ellipsis, EllipsisLen);

            NameBegin = Truncated;
            NameEnd   = Truncated + Low + EllipsisLen;
            TextSize  = ImGui::CalcTextSize(NameBegin, NameEnd, false, TileSize);
        }

        ImGuiX::Font::PopFont();

        ImGui::Dummy(ImVec2(0.0f, GLabelGap));

        const ImVec2 LabelPos = ImGui::GetCursorScreenPos();
        const float  TextX    = LabelPos.x + (TileSize - std::min(TextSize.x, TileSize)) * 0.5f;
        const ImVec4 ClipRect(LabelPos.x, LabelPos.y, LabelPos.x + TileSize, LabelPos.y + GLabelHeight);

        ImGui::GetWindowDrawList()->AddText(LabelFont, FontSize, ImVec2(TextX, LabelPos.y),
            ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f)), NameBegin, NameEnd, TileSize, &ClipRect);

        // Reserve the fixed label band so the group (and thus every row) has a uniform height.
        ImGui::Dummy(ImVec2(TileSize, GLabelHeight));

        // Type line. Reserved from the context rather than from whether THIS item has one, so a folder
        // among assets does not shorten its own cell and break the row grid.
        if (Context.bShowTypeLabels)
        {
            // Band height comes from the font, not a constant: the editor's UI scale changes the glyph
            // size but would leave a hardcoded band behind, clipping the line it is supposed to hold.
            // Measured for every item, label or not, so the band stays uniform across a row.
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Small);
            ImFont*     TypeFont      = ImGui::GetFont();
            const float TypeFontSize  = ImGui::GetFontSize();
            const float TypeBandHeight = TypeFontSize + GTypeLabelPad;

            const FStringView TypeLabel = Item->GetTypeLabel();
            if (!TypeLabel.empty())
            {
                const char* TypeBegin = TypeLabel.data();
                const char* TypeEnd   = TypeLabel.data() + TypeLabel.size();
                const ImVec2 TypeSize = ImGui::CalcTextSize(TypeBegin, TypeEnd, false, TileSize);

                const ImVec2 TypePos(LabelPos.x + (TileSize - std::min(TypeSize.x, TileSize)) * 0.5f,
                                     LabelPos.y + GLabelHeight);
                const ImVec4 TypeClip(LabelPos.x, TypePos.y, LabelPos.x + TileSize, TypePos.y + TypeBandHeight);

                ImGui::GetWindowDrawList()->AddText(TypeFont, TypeFontSize, TypePos,
                    ImGui::GetColorU32(ImVec4(0.52f, 0.55f, 0.62f, 1.0f)), TypeBegin, TypeEnd, TileSize, &TypeClip);
            }
            ImGuiX::Font::PopFont();

            ImGui::Dummy(ImVec2(TileSize, TypeBandHeight));
        }

        ImGui::EndGroup();

        // Rubber-band hit test against the whole cell, evaluated every frame of the drag rather than only
        // on entry, so shrinking the band gives items back instead of leaving them stuck selected.
        ImVec2 MarqueeMin, MarqueeMax;
        if (GetMarqueeScreenRect(MarqueeMin, MarqueeMax))
        {
            const ImVec2 CellMin = ImGui::GetItemRectMin();
            const ImVec2 CellMax = ImGui::GetItemRectMax();

            const bool bOverlaps = MarqueeMin.x <= CellMax.x && MarqueeMax.x >= CellMin.x
                                && MarqueeMin.y <= CellMax.y && MarqueeMax.y >= CellMin.y;

            const bool bInBase = bMarqueeAdditive
                && eastl::find(MarqueeBaseSelection.begin(), MarqueeBaseSelection.end(), Item) != MarqueeBaseSelection.end();

            const bool bShouldSelect = bOverlaps || bInBase;
            if (bShouldSelect != Item->IsSelected())
            {
                ToggleSelection(Item, Context);
            }
        }

        ImGui::PopID();
    }
    
    void FTileViewWidget::DrawInlineRename(const FTileViewContext& Context)
    {
        if (bRenameFocusPending)
        {
            ImGui::SetKeyboardFocusHere();
            bRenameFocusPending = false;
        }

        ImGui::SetNextItemWidth(TileSize);

        constexpr ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue
            | ImGuiInputTextFlags_AutoSelectAll
            | ImGuiInputTextFlags_CharsNoBlank;

        const bool bSubmitted = ImGui::InputText("##InlineRename", RenameBuffer, sizeof(RenameBuffer), Flags);

        const bool bActive        = ImGui::IsItemActive();
        const bool bDeactivated   = ImGui::IsItemDeactivated();
        const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const float FrameHeight   = ImGui::GetItemRectSize().y;

        // Match the fixed label band exactly: the field, one item spacing, then filler.
        const float Spacing = ImGui::GetStyle().ItemSpacing.y;
        const float Filler  = std::max(1.0f, GLabelHeight - Spacing - FrameHeight);
        ImGui::Dummy(ImVec2(TileSize, Filler));

        if (bActive)
        {
            bRenameWasActive = true;
        }

        if (bSubmitted)
        {
            CommitInlineRename(Context);
        }
        else if (bDeactivated)
        {
            // Escape reverts and deactivates; anything else (click away) commits.
            if (bEscapePressed)
            {
                CancelInlineRename();
            }
            else
            {
                CommitInlineRename(Context);
            }
        }
        else if (bRenameWasActive && !bActive)
        {
            // Focus was lost without a deactivation event (the tile was clipped out mid-edit).
            CancelInlineRename();
        }
    }

    void FTileViewWidget::BeginInlineRename(FTileViewItem* Item)
    {
        // One asset at a time. A rename field over a multi-selection either renames the wrong thing or
        // silently applies one name to many files; refusing is the only safe reading.
        if (Item != nullptr && Selections.size() > 1)
        {
            return;
        }

        RenamingItem = Item;
        bRenameFocusPending = true;
        bRenameWasActive = false;
        RenameBuffer[0] = 0;

        if (Item != nullptr)
        {
            const FStringView Name = Item->GetCachedDisplayName();
            const size_t Length = std::min(Name.size(), sizeof(RenameBuffer) - 1);
            memcpy(RenameBuffer, Name.data(), Length);
            RenameBuffer[Length] = 0;
        }
    }

    void FTileViewWidget::CancelInlineRename()
    {
        RenamingItem = nullptr;
        bRenameFocusPending = false;
        bRenameWasActive = false;
        RenameBuffer[0] = 0;
    }

    void FTileViewWidget::CommitInlineRename(const FTileViewContext& Context)
    {
        FTileViewItem* Item = RenamingItem;

        // Clear first: the callback usually dirties the tree, which frees every item.
        RenamingItem = nullptr;
        bRenameFocusPending = false;
        bRenameWasActive = false;

        if (Item != nullptr && Context.ItemRenamedFunction)
        {
            Context.ItemRenamedFunction(Item, RenameBuffer);
        }

        RenameBuffer[0] = 0;
    }

    void FTileViewWidget::SelectAndScrollTo(FTileViewItem* Item)
    {
        PendingRevealIndex = -1;

        if (Item == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < (int32)ListItems.size(); ++Index)
        {
            if (ListItems[Index] == Item)
            {
                PendingRevealIndex = Index;
                return;
            }
        }
    }

    void FTileViewWidget::ClearTree()
    {
        // Items live in Allocator, so any in-flight rename target dies here.
        CancelInlineRename();

        // Indices refer to the list being thrown away.
        PendingRevealIndex = -1;
        SelectionAnchorIndex = -1;

        Allocator.Reset();
        ListItems.clear();
    }

    bool FTileViewWidget::HandleKeyPressed(const FTileViewContext& Context, FTileViewItem& Item, ImGuiKey Key)
    {
        if (Context.KeyPressedFunction)
        {
            return Context.KeyPressedFunction(Item, Key);
        }

        return false;
    }

    void FTileViewWidget::HandleSelectionKeys(const FTileViewContext& Context)
    {
        // Typing must not fire item actions, and a rubber-band drag owns input. WantTextInput covers
        // any focused text field, including the search box, not just this widget's rename.
        if (RenamingItem != nullptr || bMarqueeActive || ImGui::GetIO().WantTextInput)
        {
            return;
        }

        // Only the focused panel acts on keys. Checked against the root so a click on any tile,
        // which focuses the child, still counts as the browser having focus.
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            return;
        }

        // Returns rather than falling through: the dispatch loop below would otherwise deliver a bare
        // 'A' to the item handler on the same press.
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            SelectAll(Context);
            return;
        }

        if (Selections.empty())
        {
            return;
        }

        FTileViewItem* Target = Selections.front();
        if (Target == nullptr)
        {
            return;
        }

        for (int Key = ImGuiKey_NamedKey_BEGIN; Key < ImGuiKey_NamedKey_END; ++Key)
        {
            if (ImGui::IsKeyPressed((ImGuiKey)Key) && HandleKeyPressed(Context, *Target, (ImGuiKey)Key))
            {
                break;
            }
        }
    }

    void FTileViewWidget::RebuildTree(const FTileViewContext& Context, bool bKeepSelections)
    {
        ASSERT(bDirty);

        TVector<FTileViewItem*> CachedSelections = Selections;
        
        ClearSelections();
        ClearTree();

        if (bKeepSelections)
        {
            for (FTileViewItem* Select : CachedSelections)
            {
                ToggleSelection(Select, Context);
            }
        }
        
        Context.RebuildTreeFunction(this);

        bDirty = false;
    }

    void FTileViewWidget::DrawItem(FTileViewItem* ItemToDraw, const FTileViewContext& Context, ImVec2 DrawSize)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        
        
        if (Context.DrawItemOverrideFunction)
        {
            FTileViewItem::EClickState ClickState = Context.DrawItemOverrideFunction(ItemToDraw);
            if (ClickState == FTileViewItem::EClickState::SingleWithCtrl)
            {
                ToggleSelection(ItemToDraw, Context);
                SetSelectionAnchor(ItemToDraw);
            }
            else if (ClickState == FTileViewItem::EClickState::SingleWithShift)
            {
                SelectRangeTo(ItemToDraw, Context);
            }
            else if (ClickState == FTileViewItem::EClickState::Single)
            {
                ClearSelections();
                ToggleSelection(ItemToDraw, Context);
                SetSelectionAnchor(ItemToDraw);
            }
            else if (ClickState == FTileViewItem::EClickState::Double)
            {
                if (Context.ItemDoubleClickedFunction)
                {
                    Context.ItemDoubleClickedFunction(ItemToDraw);
                }
            }
        }
        else
        {
            if (ImGui::Button("##", DrawSize))
            {
                ClearSelections();
                ToggleSelection(ItemToDraw, Context);
            }
        }
        
        ImGui::PopStyleVar(2);

        // Capture hover while this tile is still ImGui's current item. DrawTooltip below submits items
        // of its own, and a tooltip window does not restore the previous last-item on close, so every
        // later IsItemHovered() in this function would be testing the tooltip's contents instead of the
        // tile. That is what made right-click intermittent: it failed exactly when you had hovered long
        // enough for the tooltip to appear.
        const bool bItemHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

        if (ImGui::BeginItemTooltip())
        {
            ItemToDraw->DrawTooltip();
            ImGui::EndTooltip();
        }

        if (bItemHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ItemToDraw->HasContextMenu())
        {
            // Right-clicking outside the selection retargets it; right-clicking inside one keeps it, so
            // the menu can act on everything a marquee just gathered. Matches every file manager.
            if (!ItemToDraw->IsSelected())
            {
                ClearSelections();
                ToggleSelection(ItemToDraw, Context);
            }

            ImGui::OpenPopup("ItemContextMenu");
        }


        if (ImGui::BeginDragDropSource())
        {
            // Same rule as the context menu. Without it, dragging an unselected tile also drags whatever
            // was selected elsewhere, since the drop handler moves the whole selection.
            if (!ItemToDraw->IsSelected())
            {
                ClearSelections();
                ToggleSelection(ItemToDraw, Context);
            }

            ItemToDraw->SetDragDropPayloadData();
            if (Context.DrawItemOverrideFunction)
            {
                Context.DrawItemOverrideFunction(ItemToDraw);
            }
            ImGui::EndDragDropSource();
        }
    
        if (ImGui::BeginDragDropTarget())
        {
            if (Context.DragDropFunction)
            {
                Context.DragDropFunction(ItemToDraw, Selections);
            }
            
            ImGui::EndDragDropTarget();
        }
        
        if (ItemToDraw->HasContextMenu())
        {
            // BeginPopup, not BeginPopupContextItem: the open decision is made above off a hover state
            // captured at the right moment. Letting it re-test here would reintroduce the same bug.
            if (ImGui::BeginPopup("ItemContextMenu"))
            {
                // Copied rather than passed by reference: the menu can clear or rebuild the selection
                // (deleting is the whole point), which would resize Selections mid-iteration.
                TVector<FTileViewItem*> SelectionsToDraw;
                if (ItemToDraw->IsSelected())
                {
                    SelectionsToDraw = Selections;
                }
                else
                {
                    SelectionsToDraw.push_back(ItemToDraw);
                }

                Context.DrawItemContextMenuFunction(SelectionsToDraw);

                ImGui::EndPopup();
            }
        }
    }

    bool FTileViewWidget::GetMarqueeScreenRect(ImVec2& OutMin, ImVec2& OutMax) const
    {
        if (!bMarqueeActive)
        {
            return false;
        }

        // Content space -> screen, so the band stays anchored to the tiles while the view scrolls.
        const ImVec2 ContentOrigin(ImGui::GetWindowPos().x - ImGui::GetScrollX(),
                                   ImGui::GetWindowPos().y - ImGui::GetScrollY());
        const ImVec2 Anchor(ContentOrigin.x + MarqueeAnchorContent.x, ContentOrigin.y + MarqueeAnchorContent.y);
        const ImVec2 Cursor = ImGui::GetIO().MousePos;

        OutMin = ImVec2(std::min(Anchor.x, Cursor.x), std::min(Anchor.y, Cursor.y));
        OutMax = ImVec2(std::max(Anchor.x, Cursor.x), std::max(Anchor.y, Cursor.y));
        return true;
    }

    void FTileViewWidget::UpdateMarquee(const FTileViewContext& Context)
    {
        const ImGuiIO& IO = ImGui::GetIO();

        if (!bMarqueeActive)
        {
            // Started only from EMPTY space: a drag that began on a tile is a drag-and-drop, and a drag
            // that began outside this window belongs to whatever it started in. Checked here, at the tail
            // of Draw, because IsAnyItemHovered() is only meaningful once every tile has been submitted.
            const bool bCanStart = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
                && !ImGui::IsAnyItemHovered()
                && !ImGui::IsAnyItemActive()
                && RenamingItem == nullptr
                && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);

            if (!bCanStart)
            {
                return;
            }

            const ImVec2 ContentOrigin(ImGui::GetWindowPos().x - ImGui::GetScrollX(),
                                       ImGui::GetWindowPos().y - ImGui::GetScrollY());
            // Anchored at where the button went DOWN, not where the drag threshold was crossed, so the
            // band starts under the initial click rather than a few pixels into the gesture.
            const ImVec2 Pressed = IO.MouseClickedPos[ImGuiMouseButton_Left];

            bMarqueeActive       = true;
            bMarqueeAdditive     = IO.KeyCtrl || IO.KeyShift;
            MarqueeAnchorContent = ImVec2(Pressed.x - ContentOrigin.x, Pressed.y - ContentOrigin.y);

            MarqueeBaseSelection.clear();
            if (bMarqueeAdditive)
            {
                MarqueeBaseSelection = Selections;
            }
            else
            {
                ClearSelections();
            }
            return;
        }

        ImVec2 Min, Max;
        if (GetMarqueeScreenRect(Min, Max))
        {
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(Min, Max, IM_COL32(120, 170, 235, 40));
            DrawList->AddRect(Min, Max, IM_COL32(140, 190, 245, 200));
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            bMarqueeActive = false;
            MarqueeBaseSelection.clear();
        }
    }

    void FTileViewWidget::ToggleSelection(FTileViewItem* Item, const FTileViewContext& Context)
    {
        bool bWasSelected = Item->bSelected;
        
        if (!bWasSelected)
        {
            DEBUG_ASSERT(eastl::find(Selections.begin(), Selections.end(), Item) == Selections.end());
            Selections.push_back(Item);
            Context.ItemSelectedFunction(Item);
            Item->bSelected = true;
        }
        else
        {
            auto It = eastl::remove(Selections.begin(), Selections.end(), Item);
            Selections.erase(It);
            Item->bSelected = false;
        }

        Item->OnSelectionStateChanged();
    }

    void FTileViewWidget::SetSelectionAnchor(const FTileViewItem* Item)
    {
        const auto Found = eastl::find(ListItems.begin(), ListItems.end(), Item);
        SelectionAnchorIndex = Found == ListItems.end() ? -1 : (int32)eastl::distance(ListItems.begin(), Found);
    }

    void FTileViewWidget::SelectRangeTo(FTileViewItem* Item, const FTileViewContext& Context)
    {
        const auto Found = eastl::find(ListItems.begin(), ListItems.end(), Item);
        if (Found == ListItems.end())
        {
            return;
        }

        const int32 To = (int32)eastl::distance(ListItems.begin(), Found);

        // No anchor yet (first click in a fresh folder): shift-click behaves as a plain click and
        // becomes the anchor, so the next one has a range to span.
        if (SelectionAnchorIndex < 0 || SelectionAnchorIndex >= (int32)ListItems.size())
        {
            ClearSelections();
            ToggleSelection(Item, Context);
            SelectionAnchorIndex = To;
            return;
        }

        const int32 Low  = Math::Min(SelectionAnchorIndex, To);
        const int32 High = Math::Max(SelectionAnchorIndex, To);

        ClearSelections();
        for (int32 Index = Low; Index <= High; ++Index)
        {
            ToggleSelection(ListItems[Index], Context);
        }

        // Anchor deliberately left where it was: shift-clicking again re-spans from the same origin
        // rather than growing from wherever the last one landed.
    }

    void FTileViewWidget::SelectAll(const FTileViewContext& Context)
    {
        for (FTileViewItem* Item : ListItems)
        {
            if (!Item->IsSelected())
            {
                ToggleSelection(Item, Context);
            }
        }
    }

    void FTileViewWidget::ClearSelections()
    {
        for (FTileViewItem* Item : Selections)
        {
            ASSERT(Item->bSelected);
            
            Item->bSelected = false;
            Item->OnSelectionStateChanged();    
        }

        Selections.clear();
    }
}
