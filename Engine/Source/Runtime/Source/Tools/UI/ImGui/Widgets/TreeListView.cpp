#include "RuntimePCH.h"
#include "TreeListView.h"
#include "imgui.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        constexpr float kRowHeight = 15.0f;

        // Falls back to the plain TooltipText when no rich fields are set.
        void DrawTreeNodeTooltip(const FTreeNodeDisplay& Display)
        {
            const bool bRich = !Display.TooltipTitle.IsEmpty() || !Display.TooltipChips.IsEmpty();
            if (!bRich)
            {
                ImGuiX::TextTooltip("{}", Display.TooltipText);
                return;
            }

            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                return;
            }

            const float Scale = ImGuiX::GetUIScale();

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11.0f * Scale, 9.0f * Scale));
            if (ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None))
            {
                const char* Title = Display.TooltipTitle.IsEmpty() ? *Display.DisplayName : *Display.TooltipTitle;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 1.0f, 1.0f));
                ImGui::TextUnformatted(Title);
                ImGui::PopStyleColor();

                if (!Display.TooltipSubtitle.IsEmpty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.55f, 0.62f, 1.0f));
                    ImGui::TextUnformatted(*Display.TooltipSubtitle);
                    ImGui::PopStyleColor();
                }

                if (!Display.TooltipChips.IsEmpty())
                {
                    ImGui::Dummy(ImVec2(0.0f, 3.0f * Scale));

                    if (!Display.TooltipChipHeader.IsEmpty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.53f, 1.0f));
                        ImGui::TextUnformatted(*Display.TooltipChipHeader);
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.0f, 2.0f * Scale));
                    }

                    ImDrawList* DL = ImGui::GetWindowDrawList();
                    const ImVec2 Origin = ImGui::GetCursorScreenPos();
                    const ImVec2 ChipPad(7.0f * Scale, 3.0f * Scale);
                    const float GapX = 5.0f * Scale;
                    const float GapY = 5.0f * Scale;
                    const float Rounding = 4.0f * Scale;
                    const float WrapWidth = 340.0f * Scale;

                    const ImU32 ChipBg     = IM_COL32(54, 57, 66, 255);
                    const ImU32 ChipBorder = IM_COL32(80, 85, 96, 255);
                    const ImU32 ChipText   = IM_COL32(206, 211, 220, 255);

                    float X = 0.0f, Y = 0.0f, LineHeight = 0.0f, MaxRight = 0.0f;
                    for (const FString& Chip : Display.TooltipChips)
                    {
                        const ImVec2 TextSize = ImGui::CalcTextSize(*Chip);
                        const float W = TextSize.x + ChipPad.x * 2.0f;
                        const float H = TextSize.y + ChipPad.y * 2.0f;

                        if (X > 0.0f && X + W > WrapWidth)
                        {
                            X = 0.0f;
                            Y += LineHeight + GapY;
                            LineHeight = 0.0f;
                        }

                        const ImVec2 P0(Origin.x + X, Origin.y + Y);
                        const ImVec2 P1(P0.x + W, P0.y + H);
                        DL->AddRectFilled(P0, P1, ChipBg, Rounding);
                        DL->AddRect(P0, P1, ChipBorder, Rounding);
                        DL->AddText(ImVec2(P0.x + ChipPad.x, P0.y + ChipPad.y), ChipText, *Chip);

                        MaxRight = ImMax(MaxRight, X + W);
                        X += W + GapX;
                        LineHeight = ImMax(LineHeight, H);
                    }

                    ImGui::Dummy(ImVec2(MaxRight, Y + LineHeight));
                }

                ImGui::EndTooltip();
            }
            ImGui::PopStyleVar();
        }
    }

    FTreeListView::~FTreeListView()
    {
        for (FNode& Node : Nodes)
        {
            if (Node.bAlive && Node.UserData != nullptr && Node.UserDataDeleter != nullptr)
            {
                Node.UserDataDeleter(Node.UserData);
                Node.UserData = nullptr;
            }
        }
    }

    void FTreeListView::Draw(const FTreeListViewContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        if (bDirty)
        {
            RebuildTreeNow(Context);
        }

        if (bVisibleListDirty)
        {
            RebuildVisibleList();
        }

        // Dense row list, because the clipper measures row height from the first submitted index.
        const TVector<int32>* Rows = &VisibleList;
        if (Context.FilterFunction && Context.bFilterActive)
        {
            FilteredList.Reset();
            FilteredList.Reserve(VisibleList.Num());
            for (int32 NodeIdx : VisibleList)
            {
                if (Context.FilterFunction(*this, FTreeNodeID{NodeIdx}))
                {
                    FilteredList.Add(NodeIdx);
                }
            }
            Rows = &FilteredList;
        }

        CurrentRows = Rows;

        // Consumed either way, so a filtered or collapsed-out reveal request simply lapses.
        int32 ScrollToRow = -1;
        if (PendingScrollNode >= 0)
        {
            for (int32 i = 0; i < Rows->Num(); ++i)
            {
                if ((*Rows)[i] == PendingScrollNode)
                {
                    ScrollToRow = i;
                    break;
                }
            }
            PendingScrollNode = -1;
        }

        ImGuiTableFlags TableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX
        | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_BordersV;

        bool bAnyRowExpansionChanged = false;

        // Rows sit flush, so the height cell padding used to add moves into the node's frame padding.
        const ImGuiStyle& DrawStyle = ImGui::GetStyle();
        const ImVec2 RowFramePadding(DrawStyle.FramePadding.x, DrawStyle.FramePadding.y + 2.0f);

        ImGui::PushID(this);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, RowFramePadding);
        if (ImGui::BeginTable("TreeViewTable", 1, TableFlags))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);

            ImGuiListClipper Clipper;
            Clipper.Begin(Rows->Num());

            // The target row is off-screen by definition, and SetScrollHereY needs its real item rect.
            if (ScrollToRow != -1)
            {
                Clipper.IncludeItemByIndex(ScrollToRow);
            }

            // A row's callbacks can reparent mid-iteration, invalidating rows, nodes and the Nodes vector.
            const uint32 VersionAtStart = StructureVersion;
            bool bStructureChanged = false;

            while (!bStructureChanged && Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    // Already filtered above, so every index here submits exactly one row.
                    const int32 NodeIdx = (*Rows)[i];

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    DrawSingleRow(NodeIdx, Context, bAnyRowExpansionChanged);

                    if (i == ScrollToRow)
                    {
                        ImGui::SetScrollHereY(0.5f);
                    }

                    if (StructureVersion != VersionAtStart)
                    {
                        bStructureChanged = true;
                        bVisibleListDirty = true;
                        break;
                    }
                }
            }

            if (bStructureChanged)
            {
                Clipper.End();
            }

            ImGui::EndTable();
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        {
            ClearSelections(Context);
        }

        ImGui::PopStyleVar(2);
        ImGui::PopID();

        if (bAnyRowExpansionChanged)
        {
            bVisibleListDirty = true;
        }
    }

    void FTreeListView::ClearTree()
    {
        for (FNode& Node : Nodes)
        {
            if (Node.bAlive && Node.UserData != nullptr && Node.UserDataDeleter != nullptr)
            {
                Node.UserDataDeleter(Node.UserData);
            }
        }
        Nodes.Reset();
        FreeList.Reset();
        Roots.Reset();
        VisibleList.Reset();
        CurrentRows = nullptr;
        SelectionAnchor = -1;
        AliveCount = 0;
        bVisibleListDirty = true;
        ++StructureVersion;
    }

    void FTreeListView::RebuildTreeNow(const FTreeListViewContext& Context)
    {
        DEBUG_ASSERT(Context.RebuildTreeFunction);
        DEBUG_ASSERT(bDirty);

        THashSet<uint64> CachedExpandedItems;
        CachedExpandedItems.Reserve(AliveCount);
        for (const FNode& Node : Nodes)
        {
            if (Node.bAlive && Node.State.bExpanded)
            {
                CachedExpandedItems.TryAdd(Node.Hash);
            }
        }

        // Do NOT call ClearSelections, since a structural rebuild is not user intent.
        ClearTree();

        // Cleared first, so a refresh requested while the tree rebuilds runs again next frame.
        bDirty = false;
        Context.RebuildTreeFunction(*this);

        // Top-down, so an expanded lazy node rebuilds its children before the arrow shows open.
        TVector<int32> Stack(Roots.begin(), Roots.end());
        while (!Stack.IsEmpty())
        {
            const int32 Idx = Stack.Last();
            Stack.Pop();

            FNode& Node = Nodes[Idx];
            if (CachedExpandedItems.Contains(Node.Hash))
            {
                Node.State.bExpanded = true;
                if (Node.bHasLazyChildren && !Node.bChildrenBuilt)
                {
                    EnsureChildrenBuilt(Idx, Context); // may reallocate Nodes; do not touch Node after this
                }
            }

            for (int32 Child : Nodes[Idx].Children)
            {
                Stack.Add(Child);
            }
        }

        bVisibleListDirty = true;
    }

    void FTreeListView::RebuildVisibleList()
    {
        LUMINA_PROFILE_SCOPE();

        VisibleList.Reset();
        VisibleList.Reserve(AliveCount);

        for (int32 RootIdx : Roots)
        {
            AppendVisibleSubtree(RootIdx);
        }

        bVisibleListDirty = false;
    }

    void FTreeListView::AppendVisibleSubtree(int32 NodeIdx)
    {
        VisibleList.Add(NodeIdx);
        const FNode& Node = Nodes[NodeIdx];
        if (!Node.State.bExpanded)
        {
            return;
        }

        const int32 NumChildren = Node.Children.Num();
        for (int32 i = 0; i < NumChildren; ++i)
        {
            AppendVisibleSubtree(Nodes[NodeIdx].Children[i]);
        }
    }

    void FTreeListView::DrawSingleRow(int32 NodeIdx, const FTreeListViewContext& Context, bool& bAnyRowExpansionChanged)
    {
        FNode& Node = Nodes[NodeIdx];
        FTreeNodeDisplay& Display = Node.Display;
        FTreeNodeState& State = Node.State;
        const bool bDeclaresChildren = !Node.Children.IsEmpty() || Node.bHasLazyChildren;
        const float kIndentPerDepth = Context.IndentPerDepth;

        // Depth is cached because row callbacks can reallocate Nodes and invalidate these references.
        const int32  RowDepth       = Node.Depth;
        const uint32 VersionAtEntry = StructureVersion;

        ImGui::PushID(NodeIdx);

        if (RowDepth > 0)
        {
            ImGui::Indent(RowDepth * kIndentPerDepth);
        }

        // DrawLinesNone because this is a flattened list; the connector elbows are drawn by hand below.
        ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_DrawLinesNone
            | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        if (!bDeclaresChildren)
        {
            Flags |= ImGuiTreeNodeFlags_Leaf;
        }
        else
        {
            ImGui::SetNextItemOpen(State.bExpanded);
            // Double-click toggles anywhere on the row, so a single click can select without collapsing.
            Flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        }

        if (State.bSelected)
        {
            Flags |= ImGuiTreeNodeFlags_Selected;
        }

        ImVec4 TextColor = Display.DisplayColor;
        if (State.bDisabled)
        {
            TextColor.w *= 0.4f;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, TextColor);

        // Selection paints the table ROW, not the node's frame, so it covers the row's full height and width.
        if (State.bSelected)
        {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.22f, 0.52f, 0.22f, 0.40f)));
        }

        // Closes what this row still has open when a callback restructures the tree; mirrors the tail.
        auto UnwindRow = [&]()
        {
            if (RowDepth > 0)
            {
                ImGui::Unindent(RowDepth * kIndentPerDepth);
            }
            ImGui::PopID();
        };

        const ImVec2 RowCursorScreenPos = ImGui::GetCursorScreenPos();

        // Scoped to the node, since pushing it wider also blanks selected rows in menus and tooltips.
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const bool bNowExpanded = ImGui::TreeNodeEx("##TreeNode", Flags, "%s", *Display.DisplayName);
        ImGui::PopStyleColor();

        if (!Display.IconText.IsEmpty() && !State.bEditingText)
        {
            const ImGuiStyle& Style = ImGui::GetStyle();
            const float IconX = RowCursorScreenPos.x + ImGui::GetFontSize() + Style.FramePadding.x * 2.0f;
            const float IconY = RowCursorScreenPos.y + Style.FramePadding.y;
            ImGui::GetWindowDrawList()->AddText(ImVec2(IconX, IconY), ImGui::GetColorU32(Display.IconColor), *Display.IconText);
        }


        {
            ImDrawList* DL = ImGui::GetWindowDrawList();
            const ImGuiStyle& LineStyle = ImGui::GetStyle();
            const ImVec2 ItemMin = ImGui::GetItemRectMin();
            const ImVec2 ItemMax = ImGui::GetItemRectMax();
            // Rows are flush (CellPadding.y is forced to 0 above), so the item's own edges already meet the next row's.
            const float LineTop = ItemMin.y;
            const float LineBot = ItemMax.y;
            const float MidY    = (ItemMin.y + ItemMax.y) * 0.5f;
            const float BaseX   = RowCursorScreenPos.x - Node.Depth * kIndentPerDepth;
            const float Gutter  = ImGui::GetFontSize() * 0.5f + LineStyle.FramePadding.x;
            const ImU32 LineCol = ImGui::GetColorU32(ImGuiCol_Text, 0.45f);
            constexpr float LineThickness = 2.5f;

            if (Node.Depth > 0)
            {
                bool LastChild[64];
                const int32 MaxDepth = Node.Depth < 63 ? Node.Depth : 63;
                for (int32 d = 0; d < 64; ++d)
                {
                    LastChild[d] = true;
                }

                int32 Cur = NodeIdx;
                for (int32 d = MaxDepth; d >= 1 && Cur >= 0; --d)
                {
                    const int32 PIdx = Nodes[Cur].ParentIdx;
                    LastChild[d] = (PIdx < 0) || Nodes[PIdx].Children.IsEmpty() || (Nodes[PIdx].Children.Last() == Cur);
                    Cur = PIdx;
                }

                for (int32 d = 1; d <= MaxDepth; ++d)
                {
                    const float VLineX = BaseX + (d - 1) * kIndentPerDepth + Gutter;
                    if (d < MaxDepth)
                    {
                        // Ancestor gutter, drawn only while that ancestor still has siblings below.
                        if (!LastChild[d])
                        {
                            DL->AddLine(ImVec2(VLineX, LineTop), ImVec2(VLineX, LineBot), LineCol, LineThickness);
                        }
                    }
                    else
                    {
                        // Drop in from above, elbow across to the arrow, continue down when not last.
                        DL->AddLine(ImVec2(VLineX, LineTop), ImVec2(VLineX, MidY), LineCol, LineThickness);
                        DL->AddLine(ImVec2(VLineX, MidY), ImVec2(BaseX + d * kIndentPerDepth + Gutter, MidY), LineCol, LineThickness);
                        if (!LastChild[d])
                        {
                            DL->AddLine(ImVec2(VLineX, MidY), ImVec2(VLineX, LineBot), LineCol, LineThickness);
                        }
                    }
                }
            }

            // Drop a stub from this node's own arrow down toward its first visible child.
            if (bNowExpanded && !Node.Children.IsEmpty())
            {
                const float ChildLineX = BaseX + Node.Depth * kIndentPerDepth + Gutter;
                DL->AddLine(ImVec2(ChildLineX, MidY), ImVec2(ChildLineX, LineBot), LineCol, LineThickness);
            }
        }


        if (Context.ItemContextMenuFunction
            && ImGui::IsItemHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
            && !State.bSelected)
        {
            SetSelection(FTreeNodeID{NodeIdx}, Context, true);
        }
        
        if (Context.ItemContextMenuFunction)
        {
            ImGui::OpenPopupOnItemClick("ItemContextMenu", ImGuiPopupFlags_MouseButtonRight);
        }

        const bool bTreeNodeDoubleClicked = ImGui::IsItemHovered()
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && !ImGui::IsItemToggledOpen();

        if (bDeclaresChildren && bNowExpanded != State.bExpanded)
        {
            State.bExpanded = bNowExpanded;
            bAnyRowExpansionChanged = true;

            if (bNowExpanded && Node.bHasLazyChildren && !Node.bChildrenBuilt)
            {
                EnsureChildrenBuilt(NodeIdx, Context);
            }
        }

        if (State.bEditingText && Display.bAllowRenaming)
        {
            ImVec2 ItemMin = ImGui::GetItemRectMin();
            ImVec2 ItemMax = ImGui::GetItemRectMax();

            float ArrowWidth = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(ImVec2(ItemMin.x + ArrowWidth, ItemMin.y));

            ImGui::SetNextItemWidth(ItemMax.x - ItemMin.x - ArrowWidth);
            ImGui::SetKeyboardFocusHere();

            static char EditBuffer[256];

            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
            if (ImGui::InputText("##EditLabel", EditBuffer, sizeof(EditBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                State.bEditingText = false;
                if (Context.RenameFunction)
                {
                    Context.RenameFunction(*this, FTreeNodeID{NodeIdx}, EditBuffer);
                }

				EditBuffer[0] = '\0';
            }
            ImGui::PopStyleColor();

            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (!ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
            {
                State.bEditingText = false;
            }
        }

        ImGui::PopStyleColor(); // text color

        // Selects only; expansion belongs to the arrow and to double-click.
        bool bTreeNodeClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();

        if (ImGui::IsItemHovered())
        {
            if (Context.HoveredFunction)
            {
                Context.HoveredFunction(*this, FTreeNodeID{NodeIdx});
            }
            
            // Nothing is a shortcut while a text field owns the keyboard, or Delete kills the renamed row.
            if (!ImGui::GetIO().WantTextInput)
            {
                if (ImGui::IsKeyDown(ImGuiKey_F2) && Display.bAllowRenaming)
                {
                    State.bEditingText = true;
                }

                for (int Key = ImGuiKey_NamedKey_BEGIN; Key < ImGuiKey_NamedKey_END; Key++)
                {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(Key)))
                    {
                        if (HandleKeyPressed(Context, FTreeNodeID{NodeIdx}, (ImGuiKey)Key))
                        {
                            break;
                        }
                    }
                }
            }
        }

        if (ImGui::BeginDragDropSource())
        {
            if (Context.SetDragDropFunction)
            {
                Context.SetDragDropFunction(*this, FTreeNodeID{NodeIdx});
            }

            ImGui::TextUnformatted(*Display.DisplayName);
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget())
        {
            if (Context.DragDropFunction)
            {
                Context.DragDropFunction(*this, FTreeNodeID{NodeIdx});
            }

            ImGui::EndDragDropTarget();
        }

        // A drop reparents here, freeing nodes and reallocating Nodes, so nothing below may run.
        if (StructureVersion != VersionAtEntry)
        {
            UnwindRow();
            return;
        }

        DrawTreeNodeTooltip(Display);

        if (Context.ItemContextMenuFunction)
        {
            // Opened above via OpenPopupOnItemClick; here we only draw it.
            if (ImGui::BeginPopup("ItemContextMenu"))
            {
                Context.ItemContextMenuFunction(*this, FTreeNodeID{NodeIdx});
                ImGui::EndPopup();
            }
        }

        // Same again, since a context menu entry is just as free to delete this row's entity.
        if (StructureVersion != VersionAtEntry)
        {
            UnwindRow();
            return;
        }

        bool bMouseOverTrailingButton = false;
        {
            const int32 TrailingCount = (Display.bShowSecondaryIcon ? 1 : 0) + (Display.bShowDisabledIcon ? 1 : 0);

            if (TrailingCount > 0)
            {
                const float ButtonWidth = ImGui::GetFrameHeight();
                const float RowHeight   = ImGui::GetFrameHeight();

                ImGui::SameLine();
                const float RightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(RightEdgeX - TrailingCount * ButtonWidth);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

                if (Display.bShowSecondaryIcon)
                {
                    ImGui::PushID("##SecondaryToggle");
                    const char* Icon = State.bSecondaryToggled ? *Display.SecondaryIconOff : *Display.SecondaryIconOn;
                    // Dim while toggled off so the on/off state reads at a glance.
                    if (State.bSecondaryToggled)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    }
                    ImGui::Button(Icon, ImVec2(ButtonWidth, RowHeight));
                    if (State.bSecondaryToggled)
                    {
                        ImGui::PopStyleColor();
                    }

                    const bool bSecondaryClicked = ImGui::IsItemActivated();
                    bMouseOverTrailingButton |= ImGui::IsItemHovered();
                    ImGuiX::TextTooltip("{}", Display.SecondaryTooltip);
                    ImGui::PopID();

                    if (bSecondaryClicked)
                    {
                        State.bSecondaryToggled = !State.bSecondaryToggled;
                        if (Context.SecondaryToggleFunction)
                        {
                            Context.SecondaryToggleFunction(*this, FTreeNodeID{NodeIdx});
                        }
                    }

                    ImGui::SameLine(0.0f, 0.0f);
                }

                if (Display.bShowDisabledIcon)
                {
                    ImGui::PushID("##VisibilityToggle");
                    const char* Icon = State.bDisabled ? LE_ICON_EYE_OFF : LE_ICON_EYE;
                    ImGui::Button(Icon, ImVec2(ButtonWidth, RowHeight));
                    const bool bSmallButtonClicked = ImGui::IsItemActivated();
                    bMouseOverTrailingButton |= ImGui::IsItemHovered();
                    ImGui::PopID();

                    if (bSmallButtonClicked)
                    {
                        State.bDisabled = ~State.bDisabled;

                        // Cascade disabled state down the subtree iteratively.
                        TVector<int32> Stack(Nodes[NodeIdx].Children.begin(), Nodes[NodeIdx].Children.end());
                        while (!Stack.IsEmpty())
                        {
                            int32 Current = Stack.Last();
                            Stack.Pop();

                            Nodes[Current].State.bDisabled = State.bDisabled;
                            for (int32 Grandchild : Nodes[Current].Children)
                            {
                                Stack.Add(Grandchild);
                            }
                        }

                        if (Context.VisibilityToggleFunction)
                        {
                            Context.VisibilityToggleFunction(*this, FTreeNodeID{NodeIdx});
                        }
                    }
                }

                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
        }

        if (bTreeNodeClicked && !bMouseOverTrailingButton)
        {
            ClickRow(FTreeNodeID{NodeIdx}, ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl, Context);
        }

        if (bTreeNodeDoubleClicked && !bMouseOverTrailingButton && Context.ItemDoubleClickedFunction)
        {
            Context.ItemDoubleClickedFunction(*this, FTreeNodeID{NodeIdx});
        }

        UnwindRow();
    }

    void FTreeListView::ExpandNode(FTreeNodeID Handle, const FTreeListViewContext& Context)
    {
        if (!IsValid(Handle))
        {
            return;
        }

        // Index-based because EnsureChildrenBuilt can reallocate Nodes and invalidate a reference.
        if (!Nodes[Handle.Index].bChildrenBuilt && Nodes[Handle.Index].bHasLazyChildren)
        {
            EnsureChildrenBuilt(Handle.Index, Context);
        }

        if (!Nodes[Handle.Index].State.bExpanded)
        {
            Nodes[Handle.Index].State.bExpanded = true;
            bVisibleListDirty = true;
        }
    }

    void FTreeListView::RequestScrollToNode(FTreeNodeID Handle)
    {
        if (!IsValid(Handle))
        {
            return;
        }

        // Ancestors already exist, so they only need expanding to reach the visible list.
        for (int32 Cursor = Nodes[Handle.Index].ParentIdx; Cursor >= 0; Cursor = Nodes[Cursor].ParentIdx)
        {
            if (!Nodes[Cursor].State.bExpanded)
            {
                Nodes[Cursor].State.bExpanded = true;
                bVisibleListDirty = true;
            }
        }

        PendingScrollNode = Handle.Index;
    }

    void FTreeListView::EnsureChildrenBuilt(int32 NodeIdx, const FTreeListViewContext& Context)
    {
        if (!Context.BuildChildrenFunction)
        {
            Nodes[NodeIdx].bChildrenBuilt = true;
            return;
        }

        Context.BuildChildrenFunction(*this, FTreeNodeID{NodeIdx});
        Nodes[NodeIdx].bChildrenBuilt = true;
    }

    int32 FTreeListView::AllocNode()
    {
        int32 Idx;
        if (!FreeList.IsEmpty())
        {
            Idx = FreeList.Last();
            FreeList.Pop();
            Nodes[Idx] = FNode{};
        }
        else
        {
            Idx = Nodes.Num();
            Nodes.Emplace();
        }
        Nodes[Idx].bAlive = true;
        ++AliveCount;
        ++StructureVersion;
        return Idx;
    }

    FTreeNodeID FTreeListView::CreateNode(FTreeNodeID Parent, FStringView Name, uint64 Hash)
    {
        const int32 Idx = AllocNode();

        const uint64 NodeHash = (Hash == 0) ? Hash::GetHash64(Name.GetData(), Name.Len()) : Hash;

        bool bShouldBeDisabled = false;
        int32 ParentDepth = -1;
        if (Parent.IsValid() && Parent.Index < Nodes.Num() && Nodes[Parent.Index].bAlive)
        {
            ParentDepth = Nodes[Parent.Index].Depth;
            bShouldBeDisabled = Nodes[Parent.Index].State.bDisabled;
            Nodes[Parent.Index].Children.Add(Idx);
            // Do NOT set bChildrenBuilt, since explicit CreateNode can mix with the lazy build callback.
        }
        else
        {
            Roots.Add(Idx);
        }

        FNode& Node = Nodes[Idx];
        Node.Hash = NodeHash;
        Node.ParentIdx = (ParentDepth >= 0) ? Parent.Index : -1;
        Node.Depth = ParentDepth + 1;
        Node.Display.DisplayName = FStringView(Name.GetData(), Name.Len());
        Node.Display.TooltipText = FStringView(Name.GetData(), Name.Len());
        Node.State.bDisabled = bShouldBeDisabled;

        bVisibleListDirty = true;
        return FTreeNodeID{Idx};
    }

    void FTreeListView::RemoveNode(FTreeNodeID Handle)
    {
        if (!IsValid(Handle))
        {
            return;
        }

        UnlinkFromParent(Handle.Index);
        RemoveSubtree(Handle.Index);

        bVisibleListDirty = true;
    }

    void FTreeListView::RemoveNodes(const TVector<FTreeNodeID>& Handles)
    {
        TVector<int32> Parents;
        Parents.Reserve(Handles.Num());
        for (FTreeNodeID Handle : Handles)
        {
            if (IsValid(Handle) && !Nodes[Handle.Index].bPendingRemoval)
            {
                Nodes[Handle.Index].bPendingRemoval = true;
                Parents.Add(Nodes[Handle.Index].ParentIdx);
            }
        }
        if (Parents.IsEmpty())
        {
            return;
        }

        Algo::Sort(Parents);
        Parents.SetNum(static_cast<int32>(Algo::Unique(Parents) - Parents.begin()));
        const auto IsRemoved = [this](int32 Child) { return Nodes[Child].bPendingRemoval; };
        for (int32 ParentIdx : Parents)
        {
            TVector<int32>& Siblings = ParentIdx >= 0 ? Nodes[ParentIdx].Children : Roots;
            Siblings.RemoveAll(IsRemoved);
        }

        // A handle inside another's subtree died with it, which the alive check skips.
        for (FTreeNodeID Handle : Handles)
        {
            if (IsValid(Handle) && Nodes[Handle.Index].bPendingRemoval)
            {
                RemoveSubtree(Handle.Index);
            }
        }

        bVisibleListDirty = true;
    }

    void FTreeListView::UnlinkFromParent(int32 Idx)
    {
        const int32 ParentIdx = Nodes[Idx].ParentIdx;
        if (ParentIdx >= 0)
        {
            auto& Children = Nodes[ParentIdx].Children;
            Children.Remove(Idx);
        }
        else
        {
            Roots.Remove(Idx);
        }
    }

    void FTreeListView::RemoveSubtree(int32 RootIdx)
    {
        TVector<int32> Stack;
        Stack.Add(RootIdx);
        while (!Stack.IsEmpty())
        {
            const int32 Cur = Stack.Last();
            Stack.Pop();

            // Push children first so we can free them too.
            for (int32 Child : Nodes[Cur].Children)
            {
                Stack.Add(Child);
            }

            DestroyNodeData(Cur);
        }
    }

    void FTreeListView::DestroyNodeData(int32 Idx)
    {
        FNode& Node = Nodes[Idx];
        if (Node.UserData != nullptr && Node.UserDataDeleter != nullptr)
        {
            Node.UserDataDeleter(Node.UserData);
            Node.UserData = nullptr;
            Node.UserDataDeleter = nullptr;
        }
        Node.Children.Reset();
        Node.bAlive = false;
        Node.bChildrenBuilt = false;
        Node.bHasLazyChildren = false;
        Node.bPendingRemoval = false;
        FreeList.Add(Idx);
        --AliveCount;
        ++StructureVersion;
    }

    void FTreeListView::MarkHasLazyChildren(FTreeNodeID Handle, bool bHasLazy)
    {
        if (!IsValid(Handle))
        {
            return;
        }
        FNode& Node = Nodes[Handle.Index];
        Node.bHasLazyChildren = bHasLazy;
        if (bHasLazy)
        {
            Node.bChildrenBuilt = false;
        }
    }

    bool FTreeListView::IsValid(FTreeNodeID Handle) const
    {
        return Handle.IsValid()
            && Handle.Index < Nodes.Num()
            && Nodes[Handle.Index].bAlive;
    }

    FTreeNodeID FTreeListView::GetParentNode(FTreeNodeID Handle) const
    {
        if (!IsValid(Handle))
        {
            return InvalidTreeNode;
        }
        const int32 ParentIdx = Nodes[Handle.Index].ParentIdx;
        return ParentIdx >= 0 ? FTreeNodeID{ParentIdx} : InvalidTreeNode;
    }

    int32 FTreeListView::NumChildNodes(FTreeNodeID Handle) const
    {
        if (!Handle.IsValid())
        {
            return Roots.Num();
        }

        if (!IsValid(Handle))
        {
            return 0;
        }

        return Nodes[Handle.Index].Children.Num();
    }

    FTreeNodeID FTreeListView::GetChildNode(FTreeNodeID Handle, int32 ChildIndex) const
    {
        if (ChildIndex < 0)
        {
            return InvalidTreeNode;
        }

        if (!Handle.IsValid())
        {
            return ChildIndex < Roots.Num() ? FTreeNodeID{Roots[ChildIndex]} : InvalidTreeNode;
        }

        if (!IsValid(Handle))
        {
            return InvalidTreeNode;
        }

        const TVector<int32>& Children = Nodes[Handle.Index].Children;
        return ChildIndex < Children.Num() ? FTreeNodeID{Children[ChildIndex]} : InvalidTreeNode;
    }

    void FTreeListView::SetSelectionSilent(FTreeNodeID Handle)
    {
        if (!IsValid(Handle))
        {
            return;
        }

        for (FNode& Node : Nodes)
        {
            if (Node.bAlive)
            {
                Node.State.bSelected = false;
            }
        }

        Nodes[Handle.Index].State.bSelected = true;
    }

    void FTreeListView::SetSelection(FTreeNodeID Item, const FTreeListViewContext& Context, bool bShouldClear)
    {
        // true = plain-click (replace); false = Ctrl-click (toggle). Single-select consumers ignore bShouldClear.
        if (bShouldClear)
        {
            for (FNode& Node : Nodes)
            {
                if (Node.bAlive)
                {
                    Node.State.bSelected = false;
                }
            }

            if (IsValid(Item))
            {
                Nodes[Item.Index].State.bSelected = true;
            }
        }
        else if (IsValid(Item))
        {
            Nodes[Item.Index].State.bSelected = !Nodes[Item.Index].State.bSelected;
        }

        if (Context.ItemSelectedFunction)
        {
            Context.ItemSelectedFunction(*this, Item, bShouldClear);
        }
    }

    void FTreeListView::ClickRow(FTreeNodeID Row, bool bShift, bool bCtrl, const FTreeListViewContext& Context)
    {
        if (!IsValid(Row))
        {
            return;
        }

        // Outside Draw there is no filtered list being iterated, so the run is taken over every displayed row.
        if (CurrentRows == nullptr && bVisibleListDirty)
        {
            RebuildVisibleList();
        }
        const TVector<int32>& Rows = CurrentRows != nullptr ? *CurrentRows : VisibleList;

        const auto Target = Algo::Find(Rows, Row.Index);
        const int32 TargetRow = Target != Rows.end() ? (int32)(Target - Rows.begin()) : -1;
        const int32 AnchorRow = bShift && Context.bAllowRangeSelect && TargetRow >= 0 ? FindRangeAnchorRow(Rows, TargetRow) : -1;
        if (AnchorRow < 0)
        {
            SetSelection(Row, Context, !bCtrl);
            SelectionAnchor = Row.Index;
            return;
        }

        // The anchor stays put, so a second shift-click grows or shrinks the same run.
        SelectRange(Rows, AnchorRow, TargetRow, Context, !bCtrl);
    }

    int32 FTreeListView::FindRangeAnchorRow(const TVector<int32>& Rows, int32 TargetRow) const
    {
        // The last plain click, while it is still shown and still selected.
        if (SelectionAnchor >= 0 && SelectionAnchor != Rows[TargetRow] && IsValid(FTreeNodeID{SelectionAnchor}) && Nodes[SelectionAnchor].State.bSelected)
        {
            if (const auto It = Algo::Find(Rows, SelectionAnchor); It != Rows.end())
            {
                return (int32)(It - Rows.begin());
            }
        }

        // Otherwise the selection came from elsewhere, such as the viewport or a rebuild, and the nearest selected row stands in.
        int32 Best = -1;
        for (int32 i = 0; i < Rows.Num(); ++i)
        {
            if (i != TargetRow && Nodes[Rows[i]].bAlive && Nodes[Rows[i]].State.bSelected
                && (Best < 0 || Math::Abs(i - TargetRow) < Math::Abs(Best - TargetRow)))
            {
                Best = i;
            }
        }
        return Best;
    }

    void FTreeListView::SelectRange(const TVector<int32>& Rows, int32 FromRow, int32 ToRow, const FTreeListViewContext& Context, bool bShouldClear)
    {
        if (bShouldClear)
        {
            for (FNode& Node : Nodes)
            {
                if (Node.bAlive)
                {
                    Node.State.bSelected = false;
                }
            }
        }

        // Walked from the anchor, so the clicked row is reported last and becomes the one the details panel shows.
        bool bFirst = bShouldClear;
        const int32 Step = FromRow <= ToRow ? 1 : -1;
        for (int32 i = FromRow; i != ToRow + Step; i += Step)
        {
            const int32 NodeIdx = Rows[i];
            if (!Nodes[NodeIdx].bAlive)
            {
                continue;
            }

            // Extending must not re-report an already-selected row, which the consumer would toggle off.
            const bool bWasSelected = Nodes[NodeIdx].State.bSelected;
            Nodes[NodeIdx].State.bSelected = true;

            if (Context.ItemSelectedFunction && (bFirst || !bWasSelected))
            {
                Context.ItemSelectedFunction(*this, FTreeNodeID{NodeIdx}, bFirst);
            }

            bFirst = false;
        }
    }

    bool FTreeListView::HandleKeyPressed(const FTreeListViewContext& Context, FTreeNodeID Item, ImGuiKey Key)
    {
        if (Context.KeyPressedFunction)
        {
            return Context.KeyPressedFunction(*this, Item, Key);
        }

        return false;
    }

    void FTreeListView::ClearSelections(const FTreeListViewContext& Context)
    {
        for (FNode& Node : Nodes)
        {
            if (Node.bAlive)
            {
                Node.State.bSelected = false;
            }
        }

        if (Context.ItemSelectedFunction)
        {
            Context.ItemSelectedFunction(*this, InvalidTreeNode, true);
        }
    }

    void FTreeListView::RecomputeDepthsRecursive(int32 NodeIdx, int32 Depth)
    {
        Nodes[NodeIdx].Depth = Depth;
        for (int32 Child : Nodes[NodeIdx].Children)
        {
            RecomputeDepthsRecursive(Child, Depth + 1);
        }
    }
}
