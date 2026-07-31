#include "pch.h"
#include "TileViewWidget.h"

#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace
    {
        constexpr float GTileSpacing  = 5.0f;   // gap between cells, horizontal and vertical
        constexpr float GLabelGap     = 4.0f;   // gap between the icon button and its label
        constexpr float GLabelHeight  = 36.0f;  // fixed label band; keeps every row the same height
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

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
        ImFont*      LabelFont = ImGui::GetFont();
        const float  FontSize  = ImGui::GetFontSize();
        const ImVec2 TextSize  = ImGui::CalcTextSize(NameBegin, NameEnd, false, TileSize);
        ImGuiX::Font::PopFont();

        ImGui::Dummy(ImVec2(0.0f, GLabelGap));

        const ImVec2 LabelPos = ImGui::GetCursorScreenPos();
        const float  TextX    = LabelPos.x + (TileSize - std::min(TextSize.x, TileSize)) * 0.5f;
        const ImVec4 ClipRect(LabelPos.x, LabelPos.y, LabelPos.x + TileSize, LabelPos.y + GLabelHeight);

        ImGui::GetWindowDrawList()->AddText(LabelFont, FontSize, ImVec2(TextX, LabelPos.y),
            ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f)), NameBegin, NameEnd, TileSize, &ClipRect);

        // Reserve the fixed label band so the group (and thus every row) has a uniform height.
        ImGui::Dummy(ImVec2(TileSize, GLabelHeight));

        ImGui::EndGroup();
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
            }
            else if (ClickState == FTileViewItem::EClickState::Single)
            {
                ClearSelections();
                ToggleSelection(ItemToDraw, Context);
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
    
        if (ImGui::BeginItemTooltip())
        {
            ItemToDraw->DrawTooltip();
            ImGui::EndTooltip();
        }
        
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ItemToDraw->HasContextMenu())
        {
            ImGui::OpenPopup("ItemContextMenu");
        }

        // Never scan keys for the tile being renamed, or typing would fire item actions.
        if (ImGui::IsItemHovered() && ItemToDraw != RenamingItem)
        {
            for (int Key = ImGuiKey_NamedKey_BEGIN; Key < ImGuiKey_NamedKey_END; Key++)
            {
                if (ImGui::IsKeyPressed((ImGuiKey)Key))
                {
                    if (HandleKeyPressed(Context, *ItemToDraw, (ImGuiKey)Key))
                    {
                        break;
                    }
                }
            }
        }
        
        if (ImGui::BeginDragDropSource())
        {
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
            if (ImGui::BeginPopupContextItem("ItemContextMenu"))
            {
                TVector<FTileViewItem*> SelectionsToDraw;
                SelectionsToDraw.push_back(ItemToDraw);
                Context.DrawItemContextMenuFunction(SelectionsToDraw);
                
                ImGui::EndPopup();
            }
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
