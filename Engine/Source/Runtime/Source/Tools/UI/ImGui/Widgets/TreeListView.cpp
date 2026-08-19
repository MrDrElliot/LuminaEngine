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

        // Styled hover tooltip for a node: an accent title, a dim subtitle, and a wrapped row of
        // rounded "chip" pills. Falls back to the plain TooltipText when no rich fields are set.
        void DrawTreeNodeTooltip(const FTreeNodeDisplay& Display)
        {
            const bool bRich = !Display.TooltipTitle.empty() || !Display.TooltipChips.empty();
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
                const char* Title = Display.TooltipTitle.empty() ? Display.DisplayName.c_str() : Display.TooltipTitle.c_str();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 1.0f, 1.0f));
                ImGui::TextUnformatted(Title);
                ImGui::PopStyleColor();

                if (!Display.TooltipSubtitle.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.55f, 0.62f, 1.0f));
                    ImGui::TextUnformatted(Display.TooltipSubtitle.c_str());
                    ImGui::PopStyleColor();
                }

                if (!Display.TooltipChips.empty())
                {
                    ImGui::Dummy(ImVec2(0.0f, 3.0f * Scale));

                    if (!Display.TooltipChipHeader.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.53f, 1.0f));
                        ImGui::TextUnformatted(Display.TooltipChipHeader.c_str());
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
                        const ImVec2 TextSize = ImGui::CalcTextSize(Chip.c_str());
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
                        DL->AddText(ImVec2(P0.x + ChipPad.x, P0.y + ChipPad.y), ChipText, Chip.c_str());

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

        // Resolve the filter into a DENSE row list before the clipper starts.
        //
        // ImGuiListClipper requires every index in [DisplayStart, DisplayEnd) to submit a row: it measures
        // row height from the first one. Filtering with a `continue` inside the loop leaves the first index
        // submitting nothing, so the cursor never moves and the clipper asserts ("first item hasn't moved
        // the cursor vertically") -- which is what typing in the outliner search hit.
        //
        // Rebuilt per frame rather than folded into RebuildVisibleList, which is cached behind
        // bVisibleListDirty; the widget cannot know when the caller's filter text changed. It also makes
        // the scrollbar reflect the FILTERED count instead of leaving a long empty scroll region.
        const TVector<int32>* Rows = &VisibleList;
        if (Context.FilterFunction)
        {
            FilteredList.clear();
            FilteredList.reserve(VisibleList.size());
            for (int32 NodeIdx : VisibleList)
            {
                if (Context.FilterFunction(*this, FTreeNodeID{NodeIdx}))
                {
                    FilteredList.push_back(NodeIdx);
                }
            }
            Rows = &FilteredList;
        }

        CurrentRows = Rows;

        // Resolve a pending reveal to a row index now the row list is final. Consumed either
        // way: if the node ended up filtered or collapsed out, the request simply lapses.
        int32 ScrollToRow = -1;
        if (PendingScrollNode >= 0)
        {
            for (int32 i = 0; i < (int32)Rows->size(); ++i)
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

        // Rows sit flush: the height cell padding used to add moves into the node's own frame padding.
        const ImGuiStyle& DrawStyle = ImGui::GetStyle();
        const ImVec2 RowFramePadding(DrawStyle.FramePadding.x, DrawStyle.FramePadding.y + 2.0f);

        ImGui::PushID(this);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, RowFramePadding);
        if (ImGui::BeginTable("TreeViewTable", 1, TableFlags))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);

            ImGuiListClipper Clipper;
            Clipper.Begin(static_cast<int>(Rows->size()));

            // The target row is almost always off-screen (that's the point), so force the clipper
            // to submit it -- SetScrollHereY needs the row's real item rect.
            if (ScrollToRow != -1)
            {
                Clipper.IncludeItemByIndex(ScrollToRow);
            }

            // A row's own callbacks can restructure the tree mid-iteration: dropping an entity onto
            // another reparents it right there in DragDropFunction, which frees nodes, recycles their
            // indices and can reallocate Nodes. Everything this loop depends on -- the row-index list,
            // node liveness, parent/child links -- is invalidated by that. Stop the frame at the row that
            // did it and let the next one draw the rebuilt tree; a single half-drawn frame is the frame
            // the user released the mouse on, so it is not visible.
            const uint32 VersionAtStart = StructureVersion;
            bool bStructureChanged = false;

            while (!bStructureChanged && Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    // Already filtered above; every index here submits exactly one row, which is the
                    // clipper's contract.
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
        Nodes.clear();
        FreeList.clear();
        Roots.clear();
        VisibleList.clear();
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
        CachedExpandedItems.reserve(AliveCount);
        for (const FNode& Node : Nodes)
        {
            if (Node.bAlive && Node.State.bExpanded)
            {
                CachedExpandedItems.emplace(Node.Hash);
            }
        }

        // Do NOT call ClearSelections: structural rebuild != user intent; clients re-apply bSelected from their own source of truth.
        ClearTree();

        Context.RebuildTreeFunction(*this);

        // Restore expansion top-down. Lazy nodes that were expanded must have their children
        // rebuilt here, otherwise the arrow shows open over an empty (un-built) subtree.
        TVector<int32> Stack(Roots.begin(), Roots.end());
        while (!Stack.empty())
        {
            const int32 Idx = Stack.back();
            Stack.pop_back();

            FNode& Node = Nodes[Idx];
            if (CachedExpandedItems.find(Node.Hash) != CachedExpandedItems.end())
            {
                Node.State.bExpanded = true;
                if (Node.bHasLazyChildren && !Node.bChildrenBuilt)
                {
                    EnsureChildrenBuilt(Idx, Context); // may reallocate Nodes; do not touch Node after this
                }
            }

            for (int32 Child : Nodes[Idx].Children)
            {
                Stack.push_back(Child);
            }
        }

        bDirty = false;
        bVisibleListDirty = true;
    }

    void FTreeListView::RebuildVisibleList()
    {
        LUMINA_PROFILE_SCOPE();

        VisibleList.clear();
        VisibleList.reserve(AliveCount);

        for (int32 RootIdx : Roots)
        {
            AppendVisibleSubtree(RootIdx);
        }

        bVisibleListDirty = false;
    }

    void FTreeListView::AppendVisibleSubtree(int32 NodeIdx)
    {
        VisibleList.push_back(NodeIdx);
        const FNode& Node = Nodes[NodeIdx];
        if (!Node.State.bExpanded)
        {
            return;
        }

        const int32 NumChildren = static_cast<int32>(Node.Children.size());
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
        const bool bDeclaresChildren = !Node.Children.empty() || Node.bHasLazyChildren;
        const float kIndentPerDepth = Context.IndentPerDepth;

        // Node/Display/State are references INTO Nodes, and this row's callbacks (drop, context menu) can
        // create or destroy nodes, which reallocates that vector. Depth is cached so the unindent at the
        // bottom never has to read back through a reference the callbacks may have invalidated.
        const int32  RowDepth       = Node.Depth;
        const uint32 VersionAtEntry = StructureVersion;

        ImGui::PushID(NodeIdx);

        if (RowDepth > 0)
        {
            ImGui::Indent(RowDepth * kIndentPerDepth);
        }

        // DrawLinesNone: this is a flattened list (NoTreePushOnOpen + manual indent), so ImGui's built-in
        // tree lines have no nesting to draw from. The connector elbows are drawn by hand below instead.
        ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_DrawLinesNone
            | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        if (!bDeclaresChildren)
        {
            Flags |= ImGuiTreeNodeFlags_Leaf;
        }
        else
        {
            ImGui::SetNextItemOpen(State.bExpanded);
            // OpenOnArrow alone made the arrow the ONLY way to expand, which is a small target. Adding
            // OpenOnDoubleClick lets a double-click anywhere on the row toggle it; a single click still
            // selects (and, for the content browser, navigates) rather than collapsing what you just
            // clicked. Single-click also expands -- see the click handler below.
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
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (State.bSelected)
        {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.22f, 0.52f, 0.22f, 0.40f)));
        }

        // Closes exactly what this row still has open, for the early return taken when a callback
        // restructures the tree. Mirrors the tail of this function.
        auto UnwindRow = [&]()
        {
            ImGui::PopStyleColor(); // header color
            if (RowDepth > 0)
            {
                ImGui::Unindent(RowDepth * kIndentPerDepth);
            }
            ImGui::PopID();
        };

        const ImVec2 RowCursorScreenPos = ImGui::GetCursorScreenPos();
        const bool bNowExpanded = ImGui::TreeNodeEx("##TreeNode", Flags, "%s", Display.DisplayName.c_str());

        if (!Display.IconText.empty() && !State.bEditingText)
        {
            const ImGuiStyle& Style = ImGui::GetStyle();
            const float IconX = RowCursorScreenPos.x + ImGui::GetFontSize() + Style.FramePadding.x * 2.0f;
            const float IconY = RowCursorScreenPos.y + Style.FramePadding.y;
            ImGui::GetWindowDrawList()->AddText(ImVec2(IconX, IconY), ImGui::GetColorU32(Display.IconColor), Display.IconText.c_str());
        }


        {
            ImDrawList* DL = ImGui::GetWindowDrawList();
            const ImGuiStyle& LineStyle = ImGui::GetStyle();
            const ImVec2 ItemMin = ImGui::GetItemRectMin();
            const ImVec2 ItemMax = ImGui::GetItemRectMax();
            // Rows are flush, so a line drawn to the item's own edges already meets the next row's.
            const float RowBridge = LineStyle.CellPadding.y;
            const float LineTop = ItemMin.y - RowBridge;
            const float LineBot = ItemMax.y + RowBridge;
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
                    LastChild[d] = (PIdx < 0) || Nodes[PIdx].Children.empty() || (Nodes[PIdx].Children.back() == Cur);
                    Cur = PIdx;
                }

                for (int32 d = 1; d <= MaxDepth; ++d)
                {
                    const float VLineX = BaseX + (d - 1) * kIndentPerDepth + Gutter;
                    if (d < MaxDepth)
                    {
                        // Ancestor gutter: keep the line going only while that ancestor has siblings below.
                        if (!LastChild[d])
                        {
                            DL->AddLine(ImVec2(VLineX, LineTop), ImVec2(VLineX, LineBot), LineCol, LineThickness);
                        }
                    }
                    else
                    {
                        // This node's own level: drop in from above, elbow across to the arrow, continue down if not last.
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
            if (bNowExpanded && !Node.Children.empty())
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

        bool bTreeNodeClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();

        // Clicking a row REVEALS its children -- expand only, never collapse. Selecting a folder and
        // having it fold shut under the cursor is the annoying half of a toggle, and collapsing stays
        // available on the arrow and on double-click.
        if (bTreeNodeClicked && bDeclaresChildren && !State.bExpanded)
        {
            State.bExpanded = true;
            bAnyRowExpansionChanged = true;

            if (Node.bHasLazyChildren && !Node.bChildrenBuilt)
            {
                EnsureChildrenBuilt(NodeIdx, Context);
            }
        }

        if (ImGui::IsItemHovered())
        {
            if (Context.HoveredFunction)
            {
                Context.HoveredFunction(*this, FTreeNodeID{NodeIdx});
            }
            
            // Nothing is a shortcut while a text field owns the keyboard. Without this, Delete typed
            // into an inline rename reaches KeyPressedFunction and deletes the row being renamed.
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

            ImGui::TextUnformatted(Display.DisplayName.c_str());
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

        // Dropping onto a row reparents right here, which frees nodes, recycles their indices and can
        // reallocate Nodes. Node/Display/State all point into it, so nothing below may run.
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

        // Same again: a context menu entry is just as free to delete the entity this row belongs to.
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
                    const char* Icon = State.bSecondaryToggled ? Display.SecondaryIconOff.c_str() : Display.SecondaryIconOn.c_str();
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
                        while (!Stack.empty())
                        {
                            int32 Current = Stack.back();
                            Stack.pop_back();

                            Nodes[Current].State.bDisabled = State.bDisabled;
                            for (int32 Grandchild : Nodes[Current].Children)
                            {
                                Stack.push_back(Grandchild);
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
            const bool bShift = ImGui::GetIO().KeyShift;
            const bool bCtrl  = ImGui::GetIO().KeyCtrl;

            const bool bCanRange = bShift
                && Context.bAllowRangeSelect
                && SelectionAnchor >= 0
                && SelectionAnchor != NodeIdx
                && IsValid(FTreeNodeID{SelectionAnchor});

            if (bCanRange)
            {
                // The anchor stays put, so dragging the shift-click around grows and shrinks one run.
                SelectRange(SelectionAnchor, NodeIdx, Context, !bCtrl);
            }
            else
            {
                SetSelection(FTreeNodeID{NodeIdx}, Context, !bCtrl);
                SelectionAnchor = NodeIdx;
            }
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

        // Index-based throughout: EnsureChildrenBuilt calls back into CreateNode, which can
        // reallocate Nodes and invalidate any reference held across it.
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

        // Ancestors are already materialized (the node exists under them), so they only need
        // expanding for the row to make it into the visible list.
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
        if (!FreeList.empty())
        {
            Idx = FreeList.back();
            FreeList.pop_back();
            Nodes[Idx] = FNode{};
        }
        else
        {
            Idx = static_cast<int32>(Nodes.size());
            Nodes.emplace_back();
        }
        Nodes[Idx].bAlive = true;
        ++AliveCount;
        ++StructureVersion;
        return Idx;
    }

    FTreeNodeID FTreeListView::CreateNode(FTreeNodeID Parent, FStringView Name, uint64 Hash)
    {
        const int32 Idx = AllocNode();

        const uint64 NodeHash = (Hash == 0) ? Hash::GetHash64(Name.data(), Name.length()) : Hash;

        bool bShouldBeDisabled = false;
        int32 ParentDepth = -1;
        if (Parent.IsValid() && Parent.Index < static_cast<int32>(Nodes.size()) && Nodes[Parent.Index].bAlive)
        {
            ParentDepth = Nodes[Parent.Index].Depth;
            bShouldBeDisabled = Nodes[Parent.Index].State.bDisabled;
            Nodes[Parent.Index].Children.push_back(Idx);
            // Do NOT set bChildrenBuilt: callers may mix explicit CreateNode + lazy BuildChildrenFunction; the lazy cb must be idempotent.
        }
        else
        {
            Roots.push_back(Idx);
        }

        FNode& Node = Nodes[Idx];
        Node.Hash = NodeHash;
        Node.ParentIdx = (ParentDepth >= 0) ? Parent.Index : -1;
        Node.Depth = ParentDepth + 1;
        Node.Display.DisplayName.assign(Name.data(), Name.length());
        Node.Display.TooltipText.assign(Name.data(), Name.length());
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

    void FTreeListView::UnlinkFromParent(int32 Idx)
    {
        const int32 ParentIdx = Nodes[Idx].ParentIdx;
        if (ParentIdx >= 0)
        {
            auto& Children = Nodes[ParentIdx].Children;
            Children.erase(eastl::remove(Children.begin(), Children.end(), Idx), Children.end());
        }
        else
        {
            Roots.erase(eastl::remove(Roots.begin(), Roots.end(), Idx), Roots.end());
        }
    }

    void FTreeListView::RemoveSubtree(int32 RootIdx)
    {
        TVector<int32> Stack;
        Stack.push_back(RootIdx);
        while (!Stack.empty())
        {
            const int32 Cur = Stack.back();
            Stack.pop_back();

            // Push children first so we can free them too.
            for (int32 Child : Nodes[Cur].Children)
            {
                Stack.push_back(Child);
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
        Node.Children.clear();
        Node.bAlive = false;
        Node.bChildrenBuilt = false;
        Node.bHasLazyChildren = false;
        FreeList.push_back(Idx);
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
            && Handle.Index < static_cast<int32>(Nodes.size())
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
            return (int32)Roots.size();
        }

        if (!IsValid(Handle))
        {
            return 0;
        }

        return (int32)Nodes[Handle.Index].Children.size();
    }

    FTreeNodeID FTreeListView::GetChildNode(FTreeNodeID Handle, int32 ChildIndex) const
    {
        if (ChildIndex < 0)
        {
            return InvalidTreeNode;
        }

        if (!Handle.IsValid())
        {
            return ChildIndex < (int32)Roots.size() ? FTreeNodeID{Roots[ChildIndex]} : InvalidTreeNode;
        }

        if (!IsValid(Handle))
        {
            return InvalidTreeNode;
        }

        const TVector<int32>& Children = Nodes[Handle.Index].Children;
        return ChildIndex < (int32)Children.size() ? FTreeNodeID{Children[ChildIndex]} : InvalidTreeNode;
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

    void FTreeListView::SelectRange(int32 AnchorIdx, int32 TargetIdx, const FTreeListViewContext& Context, bool bShouldClear)
    {
        if (CurrentRows == nullptr)
        {
            return;
        }

        const TVector<int32>& Rows = *CurrentRows;

        int32 AnchorRow = -1;
        int32 TargetRow = -1;
        for (int32 i = 0; i < (int32)Rows.size(); ++i)
        {
            if (Rows[i] == AnchorIdx)
            {
                AnchorRow = i;
            }
            if (Rows[i] == TargetIdx)
            {
                TargetRow = i;
            }
        }

        // The anchor can be filtered out or collapsed away since it was clicked; then there is no run.
        if (AnchorRow < 0 || TargetRow < 0)
        {
            SetSelection(FTreeNodeID{TargetIdx}, Context, bShouldClear);
            SelectionAnchor = TargetIdx;
            return;
        }

        if (AnchorRow > TargetRow)
        {
            const int32 Swap = AnchorRow;
            AnchorRow = TargetRow;
            TargetRow = Swap;
        }

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

        // Reported through ItemSelectedFunction: the first row replaces the selection, the rest extend it.
        bool bFirst = bShouldClear;
        for (int32 i = AnchorRow; i <= TargetRow; ++i)
        {
            const int32 NodeIdx = Rows[i];
            if (!Nodes[NodeIdx].bAlive)
            {
                continue;
            }

            // Extending must not re-report an already-selected row: the consumer would toggle it back off.
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
