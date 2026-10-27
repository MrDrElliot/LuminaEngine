#include "GraphEditor.h"

#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMinZoom             = 0.1f;
        constexpr float kMaxZoom             = 4.0f;
        constexpr float kWheelZoomStep       = 1.15f;
        constexpr float kZoomResponse        = 18.0f;
        constexpr float kGridSpacing         = 32.0f;
        constexpr int32 kMajorGridEvery      = 8;
        constexpr float kDragThreshold       = 4.0f;
        constexpr float kWireHitSlop         = 5.0f;
        constexpr float kCommentEdgeSlop     = 6.0f;
        constexpr float kCommentCornerGrip   = 16.0f;
        constexpr float kWireBend            = 100.0f;
        constexpr float kWireBendPerForward  = 0.5f;
        constexpr float kWireBendPerVertical = 0.2f;
        constexpr float kWireMaxBend         = 420.0f;
        constexpr float kTieLaneGap          = 2.0f;
        constexpr float kTieRun              = 22.0f;
        constexpr float kTieBandWidth        = 7.0f;
        constexpr float kTieHeadWidth        = 12.0f;
        constexpr float kTieHeadHeight       = 9.0f;
        constexpr float kTiePadding          = 3.0f;
        constexpr float kTieSeconds          = 0.35f;
        constexpr int32 kNodeShadowLayers    = 4;
        constexpr float kNodeShadowSpread    = 3.0f;
        constexpr float kNodeShadowDrop      = 4.0f;
        constexpr ImU32 kNodeShadow          = IM_COL32(0, 0, 0, 22);
        constexpr float kSelectionGlowWidth  = 5.0f;
        constexpr float kSpliceSlop          = 10.0f;
        constexpr float kMinimapWidthEm      = 13.0f;
        constexpr float kMinimapHeightEm     = 8.0f;
        constexpr float kMinimapMargin       = 12.0f;
        constexpr float kMinimapPadding      = 6.0f;
        constexpr float kBadgeFontScale      = 0.78f;
        constexpr float kBadgeMinZoom        = 0.45f;
        constexpr float kFlowSpacing         = 28.0f;
        constexpr float kFlowSpeed           = 70.0f;

        constexpr int32 kCommentChannel      = 1;
        constexpr int32 kWireChannel         = 2;
        constexpr int32 kFirstNodeChannel    = 3;

        constexpr uint8 kResizeLeft          = 1 << 0;
        constexpr uint8 kResizeRight         = 1 << 1;
        constexpr uint8 kResizeTop           = 1 << 2;
        constexpr uint8 kResizeBottom        = 1 << 3;

        using EditorColors::ScaleAlpha;

        ImU32 Brighten(ImU32 Color, float Amount)
        {
            return EditorColors::U32(EditorColors::Lighten(ImGui::ColorConvertU32ToFloat4(Color), Amount));
        }

        float EaseOutCubic(float T)
        {
            const float Remaining = 1.0f - T;
            return 1.0f - Remaining * Remaining * Remaining;
        }

        // Overshoots a little before settling, which reads as the tie being pulled tight.
        float EaseOutBack(float T)
        {
            constexpr float Overshoot = 1.70158f;
            const float Remaining = T - 1.0f;
            return 1.0f + (Overshoot + 1.0f) * Remaining * Remaining * Remaining + Overshoot * Remaining * Remaining;
        }

        // How far a wire runs straight out of its pins, growing with the gap so long wires curve in late.
        float WireBend(ImVec2 Start, ImVec2 End)
        {
            const float HalfDistance = Math::Sqrt(ImLengthSqr(End - Start)) * 0.5f;
            float Bend = HalfDistance < kWireBend ? kWireBend * Math::Sin(IM_PI * 0.5f * HalfDistance / kWireBend) : kWireBend;
            const float Reach = Math::Max(End.x - Start.x, 0.0f) * kWireBendPerForward + Math::Abs(End.y - Start.y) * kWireBendPerVertical;
            Bend = Math::Max(Bend, Math::Min(Reach, kWireMaxBend));
            if (End.x < Start.x)
            {
                Bend = Math::Max(Bend, Math::Min((Start.x - End.x) * 0.5f + 40.0f, 220.0f));
            }
            return Bend;
        }

        void AppendWireCurve(TVector<ImVec2>& Out, ImVec2 Start, ImVec2 End, int32 Segments, bool bIncludeStart)
        {
            const float  Bend = WireBend(Start, End);
            const ImVec2 StartControl(Start.x + Bend, Start.y);
            const ImVec2 EndControl(End.x - Bend, End.y);
            for (int32 Index = bIncludeStart ? 0 : 1; Index <= Segments; ++Index)
            {
                Out.push_back(ImBezierCubicCalc(Start, StartControl, EndControl, End, (float)Index / (float)Segments));
            }
        }

        // Curves from Start through a short straight run at each tie center to End.
        void AppendTiedRoute(TVector<ImVec2>& Out, ImVec2 Start, ImVec2 End, const TVector<ImVec2>& Centers, int32 Segments)
        {
            const int32 CurveSegments = Math::Max(Segments / ((int32)Centers.size() + 1), 8);
            ImVec2 From = Start;
            for (const ImVec2& Center : Centers)
            {
                const ImVec2 RunStart(Center.x - kTieRun, Center.y);
                AppendWireCurve(Out, From, RunStart, CurveSegments, Out.empty());
                From = ImVec2(Center.x + kTieRun, Center.y);
                Out.push_back(Center);
                Out.push_back(From);
            }
            AppendWireCurve(Out, From, End, CurveSegments, Out.empty());
        }

        // Count points evenly spaced by length, so two routes of different point counts blend point for point.
        void ResampleByLength(const TVector<ImVec2>& Path, int32 Count, TVector<ImVec2>& Out)
        {
            float Total = 0.0f;
            for (size_t Index = 1; Index < Path.size(); ++Index)
            {
                Total += Math::Sqrt(ImLengthSqr(Path[Index] - Path[Index - 1]));
            }

            Out.clear();
            size_t Segment = 1;
            float  Walked  = 0.0f;
            for (int32 Point = 0; Point < Count; ++Point)
            {
                const float Target = Total * (float)Point / (float)(Count - 1);
                float Length = Segment < Path.size() ? Math::Sqrt(ImLengthSqr(Path[Segment] - Path[Segment - 1])) : 0.0f;
                while (Segment + 1 < Path.size() && Walked + Length < Target)
                {
                    Walked += Length;
                    ++Segment;
                    Length = Math::Sqrt(ImLengthSqr(Path[Segment] - Path[Segment - 1]));
                }
                const float Fraction = Length > 0.0f ? Math::Clamp((Target - Walked) / Length, 0.0f, 1.0f) : 0.0f;
                Out.push_back(Path[Segment - 1] + (Path[Math::Min(Segment, Path.size() - 1)] - Path[Segment - 1]) * Fraction);
            }
        }

        bool SegmentsCross(ImVec2 A, ImVec2 B, ImVec2 C, ImVec2 D, ImVec2& OutPoint)
        {
            const ImVec2 R = B - A;
            const ImVec2 S = D - C;
            const float Denominator = R.x * S.y - R.y * S.x;
            if (Math::Abs(Denominator) < 1e-6f)
            {
                return false;
            }

            const ImVec2 AC = C - A;
            const float T = (AC.x * S.y - AC.y * S.x) / Denominator;
            const float U = (AC.x * R.y - AC.y * R.x) / Denominator;
            if (T < 0.0f || T > 1.0f || U < 0.0f || U > 1.0f)
            {
                return false;
            }

            OutPoint = A + R * T;
            return true;
        }

        ImRect BoundsOf(const ImVec2* Points, int32 Count)
        {
            ImRect Bounds(Points[0], Points[0]);
            for (int32 Index = 1; Index < Count; ++Index)
            {
                Bounds.Add(Points[Index]);
            }
            return Bounds;
        }

        ImVec4 GetStrokeColor(EGraphStroke Stroke)
        {
            switch (Stroke)
            {
                case EGraphStroke::Cut:     return EditorColors::Danger();
                case EGraphStroke::Reroute: return EditorColors::Success();
                default:                    return EditorColors::Accent();
            }
        }

        const char* GetStrokeLabel(EGraphStroke Stroke)
        {
            switch (Stroke)
            {
                case EGraphStroke::Cut:     return "Cut";
                case EGraphStroke::Reroute: return "Reroute";
                default:                    return "Zip tie";
            }
        }
    }

    void FGraphEditor::Begin(const char* ID, int32 NodeCount)
    {
        Frame = ImGui::GetFrameCount();

        ContextRequest      = FGraphContextRequest();
        DoubleClickedNode   = 0;
        DoubleClickedLink   = 0;
        BreakLinksPin       = 0;
        bBackgroundClicked  = false;
        Shortcuts           = 0;
        FinishedStroke      = EGraphStroke::None;
        ReportedHoveredLink = 0;
        SpliceDrop          = FGraphSplice();
        Tooltip.clear();
        if (bLinkDragActive && LinkDrag.bDropped)
        {
            bLinkDragActive = false;
            LinkDrag = FGraphLinkDrag();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild(ID, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();

        const ImVec2 ScreenMin  = ImGui::GetCursorScreenPos();
        const ImVec2 ScreenSize = ImMax(ImGui::GetContentRegionAvail(), ImVec2(1.0f, 1.0f));
        const ImRect ScreenRect(ScreenMin, ScreenMin + ScreenSize);
        Canvas.SetScreenRect(ScreenRect);

        UpdateView();
        UpdateShortcuts();

        DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(ScreenRect.Min, ScreenRect.Max, EditorColors::U32(EditorColors::Lighten(EditorColors::WindowBg(), -0.035f)));

        const float  Zoom       = Canvas.GetZoom();
        const ImRect Visible    = Canvas.GetVisibleRect();
        const ImVec4 Line       = EditorColors::TextMuted();
        const float  MinorAlpha = Math::Saturate((kGridSpacing * Zoom - 6.0f) / 10.0f) * 0.06f;
        for (int32 Axis = 0; Axis < 2; ++Axis)
        {
            const float First = Math::Floor((Axis == 0 ? Visible.Min.x : Visible.Min.y) / kGridSpacing);
            const float Last  = Axis == 0 ? Visible.Max.x : Visible.Max.y;
            for (float Step = First; Step * kGridSpacing <= Last; Step += 1.0f)
            {
                const bool  bMajor = ((int64)Step % kMajorGridEvery) == 0;
                const float Alpha  = bMajor ? 0.12f : MinorAlpha;
                if (Alpha <= 0.0f)
                {
                    continue;
                }

                const ImVec2 Point = Canvas.CanvasToScreen(ImVec2(Step * kGridSpacing, Step * kGridSpacing));
                const ImVec2 From  = Axis == 0 ? ImVec2(Point.x, ScreenRect.Min.y) : ImVec2(ScreenRect.Min.x, Point.y);
                const ImVec2 To    = Axis == 0 ? ImVec2(Point.x, ScreenRect.Max.y) : ImVec2(ScreenRect.Max.x, Point.y);
                DrawList->AddLine(From, To, EditorColors::U32(EditorColors::WithAlpha(Line, Alpha)));
            }
        }

        const ImVec2 Mouse = Canvas.ScreenToCanvas(ImGui::GetIO().MousePos);
        if (Action == EAction::DragItems || Action == EAction::ResizeComment)
        {
            DragDelta = Mouse - Press.CanvasPos;
        }

        TopNodeUnderMouse = 0;
        int32 TopRank = -1;
        for (const auto& [NodeID, State] : Nodes)
        {
            if (!State.bComment && State.Rank > TopRank && State.GetRect().Contains(Mouse))
            {
                TopRank = State.Rank;
                TopNodeUnderMouse = NodeID;
            }
        }

        RankCount = Math::Max(NodeCount, 1);
        AssignRanks();

        Canvas.Begin();
        Splitter.Split(DrawList, kFirstNodeChannel + RankCount * 2 + 1);
        SetChannel(kWireChannel);

        Pins.clear();
        Wires.clear();
        FrameComments.clear();
    }

    void FGraphEditor::UpdateView()
    {
        const ImGuiIO& IO     = ImGui::GetIO();
        const ImRect   Screen = Canvas.GetScreenRect();
        const ImVec2   Half   = Screen.GetSize() * 0.5f;
        auto SetViewAround = [this, &Half](ImVec2 Center, float Zoom)
        {
            Canvas.SetView(Center - Half / Zoom, Zoom);
        };

        if (bNavigationPending)
        {
            bNavigationPending = false;
            bZooming = false;

            const ImVec2 TargetSize = ImMax(NavigationTarget.GetSize(), ImVec2(1.0f, 1.0f));
            NavigationToZoom   = NavigationExactZoom > 0.0f
                ? Math::Clamp(NavigationExactZoom, kMinZoom, kMaxZoom)
                : Math::Clamp(Math::Min(Screen.GetWidth() / TargetSize.x, Screen.GetHeight() / TargetSize.y), kMinZoom, 1.0f);
            NavigationToCenter = NavigationTarget.GetCenter();
            TargetZoom         = NavigationToZoom;

            if (NavigationDuration <= 0.0f || !bHasDrawn)
            {
                bNavigating = false;
                SetViewAround(NavigationToCenter, NavigationToZoom);
            }
            else
            {
                bNavigating          = true;
                NavigationTime       = 0.0f;
                NavigationFromZoom   = Canvas.GetZoom();
                NavigationFromCenter = Canvas.ScreenToCanvas(Screen.GetCenter());
            }
        }

        if (bNavigating)
        {
            NavigationTime += IO.DeltaTime;
            const float Blend = Math::SmoothStep(0.0f, 1.0f, Math::Saturate(NavigationTime / NavigationDuration));
            const float Zoom  = Math::Exp(Math::Lerp(Math::Log(NavigationFromZoom), Math::Log(NavigationToZoom), Blend));
            SetViewAround(NavigationFromCenter + (NavigationToCenter - NavigationFromCenter) * Blend, Zoom);
            bNavigating = Blend < 1.0f;
            TargetZoom  = Zoom;
        }

        if (bCanvasHovered && IO.MouseWheel != 0.0f && Action != EAction::DragLink)
        {
            bNavigating      = false;
            bZooming         = true;
            TargetZoom       = Math::Clamp(TargetZoom * Math::Pow(kWheelZoomStep, IO.MouseWheel), kMinZoom, kMaxZoom);
            ZoomAnchorScreen = IO.MousePos;
            ZoomAnchorCanvas = Canvas.ScreenToCanvas(IO.MousePos);
        }

        if (bZooming)
        {
            const float Blend = 1.0f - Math::Exp(-IO.DeltaTime * kZoomResponse);
            float Zoom = Math::Exp(Math::Lerp(Math::Log(Canvas.GetZoom()), Math::Log(TargetZoom), Blend));
            if (Math::Abs(Zoom - TargetZoom) < TargetZoom * 0.001f)
            {
                Zoom = TargetZoom;
                bZooming = false;
            }
            Canvas.SetView(ZoomAnchorCanvas - (ZoomAnchorScreen - Screen.Min) / Zoom, Zoom);
        }

        if (Action == EAction::Pan)
        {
            bNavigating = false;
            Canvas.SetView(Canvas.GetOrigin() - IO.MouseDelta / Canvas.GetZoom(), Canvas.GetZoom());
            ZoomAnchorCanvas = Canvas.ScreenToCanvas(ZoomAnchorScreen);
        }
    }

    void FGraphEditor::UpdateShortcuts()
    {
        const ImGuiIO& IO = ImGui::GetIO();
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || IO.WantTextInput)
        {
            return;
        }

        // Checked ahead of the active item test, since the canvas itself is the active item while something is being dragged.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            CancelAction();
        }

        if (ImGui::IsAnyItemActive())
        {
            return;
        }

        if (IO.KeyCtrl)
        {
            Shortcuts |= ImGui::IsKeyPressed(ImGuiKey_C, false) ? (uint8)EGraphShortcut::Copy      : 0;
            Shortcuts |= ImGui::IsKeyPressed(ImGuiKey_X, false) ? (uint8)EGraphShortcut::Cut       : 0;
            Shortcuts |= ImGui::IsKeyPressed(ImGuiKey_V, false) ? (uint8)EGraphShortcut::Paste     : 0;
            Shortcuts |= ImGui::IsKeyPressed(ImGuiKey_D, false) ? (uint8)EGraphShortcut::Duplicate : 0;
            if (ImGui::IsKeyPressed(ImGuiKey_A, false))
            {
                for (const auto& [NodeID, State] : Nodes)
                {
                    SelectedNodes.insert(NodeID);
                }
            }
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        {
            Shortcuts |= (uint8)EGraphShortcut::Delete;
        }

        if (!IO.KeyShift && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            if (SelectedNodes.empty())
            {
                NavigateToContent(0.25f);
            }
            else
            {
                NavigateToSelection(0.25f);
            }
        }
    }

    void FGraphEditor::AssignRanks()
    {
        TVector<FNodeState*> Ordered;
        Ordered.reserve(Nodes.size());
        for (auto& [NodeID, State] : Nodes)
        {
            if (!State.bComment)
            {
                Ordered.push_back(&State);
            }
        }

        Algo::Sort(Ordered, [](const FNodeState* A, const FNodeState* B) { return A->ZOrder < B->ZOrder; });
        for (int32 Index = 0; Index < (int32)Ordered.size(); ++Index)
        {
            Ordered[Index]->Rank = Math::Min(Index, RankCount - 1);
        }
        NextRank = (int32)Ordered.size();
    }

    int32 FGraphEditor::GetNodeChannel(const FNodeState& State, bool bBackground) const
    {
        if (State.bComment)
        {
            return kCommentChannel;
        }
        return kFirstNodeChannel + State.Rank * 2 + (bBackground ? 0 : 1);
    }

    void FGraphEditor::BeginNode(int64 NodeID, ImVec2& InOutPosition, const FGraphNodeStyle& Style)
    {
        auto [Iterator, bAdded] = Nodes.try_emplace(NodeID);
        FNodeState& State = Iterator->second;
        if (bAdded)
        {
            State.ZOrder = NextZOrder++;
            State.Rank   = Math::Min(NextRank++, RankCount - 1);
        }

        if (auto Start = DragStartNodes.find(NodeID); Start != DragStartNodes.end())
        {
            InOutPosition = Start->second + DragDelta;
        }

        State.Position  = InOutPosition;
        State.Style     = Style;
        State.LastFrame = Frame;
        State.bComment  = false;
        CurrentNode     = NodeID;

        SetChannel(GetNodeChannel(State, false));
        ImGui::PushID((const void*)(intptr_t)NodeID);

        // A widget under a node drawn above this one would otherwise react to a cursor that is over that node.
        bMouseHiddenFromNode = TopNodeUnderMouse != 0 && TopNodeUnderMouse != NodeID && ImGui::GetActiveID() == 0;
        if (bMouseHiddenFromNode)
        {
            HiddenMousePos = ImGui::GetIO().MousePos;
            ImGui::GetIO().MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        }

        ImGui::SetCursorScreenPos(InOutPosition + ImVec2(Style.Padding.x, Style.Padding.y));
        ImGui::BeginGroup();
    }

    void FGraphEditor::EndNode()
    {
        ImGui::EndGroup();

        FNodeState& State = Nodes[CurrentNode];
        const ImVec2 ContentMax = ImGui::GetItemRectMax();
        State.Size = ImVec2(ContentMax.x + State.Style.Padding.z, ContentMax.y + State.Style.Padding.w) - State.Position;

        if (bMouseHiddenFromNode)
        {
            ImGui::GetIO().MousePos = HiddenMousePos;
            bMouseHiddenFromNode = false;
        }
        ImGui::PopID();

        const ImRect Rect = State.GetRect();
        if (ImGui::IsRectVisible(Rect.Min, Rect.Max))
        {
            const FGraphNodeStyle& Style = State.Style;
            const float Fade = ImGui::GetStyle().Alpha;

            SetChannel(GetNodeChannel(State, true));
            if ((Style.Background & IM_COL32_A_MASK) != 0)
            {
                // Wider, fainter layers stack into a soft shadow that lifts the node off the grid.
                for (int32 Layer = kNodeShadowLayers; Layer > 0; --Layer)
                {
                    const float Spread = kNodeShadowSpread * (float)Layer;
                    DrawList->AddRectFilled(Rect.Min + ImVec2(-Spread, kNodeShadowDrop - Spread), Rect.Max + ImVec2(Spread, kNodeShadowDrop + Spread),
                        ScaleAlpha(kNodeShadow, Fade), Style.Rounding + Spread);
                }
            }
            DrawList->AddRectFilled(Rect.Min, Rect.Max, ScaleAlpha(Style.Background, Fade), Style.Rounding);

            const bool bSelected = IsNodeSelected(CurrentNode);
            const bool bHovered  = HoveredNode == CurrentNode;
            ImU32 BorderColor = ScaleAlpha(Style.Border, Fade);
            float BorderWidth = Style.BorderWidth;
            if (bSelected)
            {
                BorderColor = EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), Fade));
                BorderWidth = Style.SelectedBorderWidth;
            }
            else if (bHovered)
            {
                BorderColor = EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.6f * Fade));
                BorderWidth = Style.HoveredBorderWidth;
            }

            // Drawn over the node's own header, outside its edge when highlighted so the header keeps its corners.
            SetChannel(GetNodeChannel(State, false));
            if (BorderWidth > 0.0f && (BorderColor & IM_COL32_A_MASK) != 0)
            {
                const float Grow = bSelected || bHovered ? BorderWidth * 0.5f : 0.0f;
                DrawList->AddRect(Rect.Min - ImVec2(Grow, Grow), Rect.Max + ImVec2(Grow, Grow), BorderColor, Style.Rounding + Grow, BorderWidth);
                if (bSelected)
                {
                    const float Glow = BorderWidth + kSelectionGlowWidth * 0.5f;
                    DrawList->AddRect(Rect.Min - ImVec2(Glow, Glow), Rect.Max + ImVec2(Glow, Glow), ScaleAlpha(BorderColor, 0.22f), Style.Rounding + Glow, kSelectionGlowWidth);
                }
            }
        }

        CurrentNode = 0;
    }

    void FGraphEditor::Comment(int64 NodeID, ImVec2& InOutPosition, ImVec2& InOutBodySize, float HeaderHeight, ImVec2 MinBodySize)
    {
        FNodeState& State = Nodes[NodeID];

        if (auto Start = DragStartNodes.find(NodeID); Start != DragStartNodes.end())
        {
            InOutPosition = Start->second + DragDelta;
        }

        if (Action == EAction::ResizeComment && Press.ID == (uint64)NodeID)
        {
            ImVec2 Position = ResizeStartPosition;
            ImVec2 Size     = ResizeStartSize;
            if (Press.ResizeEdges & kResizeRight)
            {
                Size.x = Math::Max(ResizeStartSize.x + DragDelta.x, MinBodySize.x);
            }
            if (Press.ResizeEdges & kResizeBottom)
            {
                Size.y = Math::Max(ResizeStartSize.y + DragDelta.y, MinBodySize.y);
            }
            if (Press.ResizeEdges & kResizeLeft)
            {
                Size.x = Math::Max(ResizeStartSize.x - DragDelta.x, MinBodySize.x);
                Position.x = ResizeStartPosition.x + ResizeStartSize.x - Size.x;
            }
            if (Press.ResizeEdges & kResizeTop)
            {
                Size.y = Math::Max(ResizeStartSize.y - DragDelta.y, MinBodySize.y);
                Position.y = ResizeStartPosition.y + ResizeStartSize.y - Size.y;
            }
            InOutPosition = Position;
            InOutBodySize = Size;
        }

        State.Position     = InOutPosition;
        State.Size         = ImVec2(InOutBodySize.x, HeaderHeight + InOutBodySize.y);
        State.HeaderHeight = HeaderHeight;
        State.MinBodySize  = MinBodySize;
        State.LastFrame    = Frame;
        State.bComment     = true;
        FrameComments.push_back(NodeID);

        SetChannel(kCommentChannel);
    }

    void FGraphEditor::BeginPin(uint32 PinID, EGraphPinKind Kind, ImU32 Color)
    {
        CurrentPin      = PinID;
        CurrentPinState = FPinState{ CurrentNode, ImRect(), ImVec2(), Kind, Color };
        ImGui::BeginGroup();
    }

    void FGraphEditor::EndPin()
    {
        ImGui::EndGroup();

        const ImRect Rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        CurrentPinState.Rect   = Rect;
        CurrentPinState.Anchor = ImVec2(CurrentPinState.Kind == EGraphPinKind::Input ? Rect.Min.x : Rect.Max.x, (Rect.Min.y + Rect.Max.y) * 0.5f);
        Pins[CurrentPin] = CurrentPinState;
        CurrentPin = 0;
    }

    void FGraphEditor::Pin(uint32 PinID, EGraphPinKind Kind, ImU32 Color, const ImRect& HitRect, ImVec2 Anchor)
    {
        Pins[PinID] = FPinState{ CurrentNode, HitRect, Anchor, Kind, Color };
    }

    void FGraphEditor::Link(uint64 LinkID, uint32 OutputPin, uint32 InputPin, ImU32 Color, float Thickness, bool bFlow)
    {
        FWire& Wire    = Wires.emplace_back();
        Wire.ID        = LinkID;
        Wire.OutputPin = OutputPin;
        Wire.InputPin  = InputPin;
        Wire.Color     = Color;
        Wire.Thickness = Thickness;
        Wire.bFlow     = bFlow;
    }

    void FGraphEditor::LinkBadge(FStringView Text, ImU32 Color)
    {
        if (!Wires.empty())
        {
            Wires.back().Badge.assign(Text.data(), Text.size());
            Wires.back().BadgeColor = Color;
        }
    }

    void FGraphEditor::GetView(ImVec2& OutCenter, float& OutZoom) const
    {
        OutCenter = Canvas.ScreenToCanvas(Canvas.GetScreenRect().GetCenter());
        OutZoom   = Canvas.GetZoom();
    }

    void FGraphEditor::ZipTie(uint64 TieID, ImVec2& InOutPosition, const TVector<uint64>& LinkIDs)
    {
        auto [Iterator, bAdded] = Ties.try_emplace(TieID);
        FZipTieState& State = Iterator->second;

        // A graph opened with ties already in it shows them tied, rather than every one animating in at once.
        if (bAdded && !bHasDrawn)
        {
            State.Amount = 1.0f;
        }

        if (auto Start = DragStartTies.find(TieID); Start != DragStartTies.end())
        {
            InOutPosition = Start->second + DragDelta;
        }

        State.Position  = InOutPosition;
        State.Links     = LinkIDs;
        State.LastFrame = Frame;
    }

    ImDrawList* FGraphEditor::GetNodeBackgroundDrawList(int64 NodeID)
    {
        auto Iterator = Nodes.find(NodeID);
        if (Iterator == Nodes.end() || Iterator->second.LastFrame != Frame)
        {
            return nullptr;
        }

        SetChannel(GetNodeChannel(Iterator->second, true));
        return DrawList;
    }

    ImVec2 FGraphEditor::GetNodeSize(int64 NodeID) const
    {
        auto Iterator = Nodes.find(NodeID);
        return Iterator != Nodes.end() ? Iterator->second.Size : ImVec2(0.0f, 0.0f);
    }

    void FGraphEditor::End()
    {
        const float DeltaTime = ImGui::GetIO().DeltaTime;
        for (auto& [TieID, Tie] : Ties)
        {
            const bool  bLive    = Tie.LastFrame == Frame;
            const float Previous = Tie.Amount;
            Tie.Amount = Math::Saturate(Tie.Amount + (bLive ? DeltaTime : -DeltaTime) / kTieSeconds);
            Tie.Flash  = Previous < 1.0f && Tie.Amount >= 1.0f ? 1.0f : Math::Max(Tie.Flash - DeltaTime * 4.0f, 0.0f);
        }
        erase_if(Ties, [this](const auto& Pair) { return Pair.second.LastFrame != Frame && Pair.second.Amount <= 0.0f; });

        BuildWires();

        // Catches every press that no widget inside a node took for itself.
        const ImRect Visible = Canvas.GetVisibleRect();
        ImGui::SetCursorScreenPos(Visible.Min);
        ImGui::InvisibleButton("##GraphCanvas", ImMax(Visible.GetSize(), ImVec2(1.0f, 1.0f)),
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bCanvasHovered = ImGui::IsItemHovered();
        const bool bCanvasActive = ImGui::IsItemActive();
        if (bCanvasHovered)
        {
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        }

        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        bMouseOverMinimap = UpdateMinimap(Canvas.GetScreenMousePos());
        if ((bCanvasHovered || bCanvasActive) && !bMouseOverMinimap)
        {
            HitTest(Mouse);
        }
        else
        {
            HoveredItem        = EGraphItem::None;
            HoveredNode        = 0;
            HoveredPin         = 0;
            HoveredLink        = 0;
            HoveredTie         = 0;
            HoveredResizeEdges = 0;
        }

        ProcessInput(Mouse);

        const uint8 Edges = Action == EAction::ResizeComment ? Press.ResizeEdges : HoveredResizeEdges;
        if (Edges != 0)
        {
            const bool bHorizontal = (Edges & (kResizeLeft | kResizeRight)) != 0;
            const bool bVertical   = (Edges & (kResizeTop | kResizeBottom)) != 0;
            const bool bFalling    = (Edges & (kResizeLeft | kResizeTop)) == (kResizeLeft | kResizeTop) || (Edges & (kResizeRight | kResizeBottom)) == (kResizeRight | kResizeBottom);
            ImGui::SetMouseCursor(bHorizontal && bVertical ? (bFalling ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW) : (bHorizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS));
        }

        SetChannel(kWireChannel);
        DrawWires();
        DrawZipTies();
        SetChannel(kFirstNodeChannel + RankCount * 2);
        DrawOverlay();

        Splitter.Merge(DrawList);
        Canvas.End();
        DrawMinimap();

        erase_if(Nodes, [this](const auto& Pair) { return Pair.second.LastFrame != Frame; });
        erase_if(SelectedNodes, [this](int64 NodeID) { return !Nodes.contains(NodeID); });
        erase_if(SelectedTies, [this](uint64 TieID) { auto Tie = Ties.find(TieID); return Tie == Ties.end() || Tie->second.LastFrame != Frame; });
        if (!SelectedLinks.empty())
        {
            erase_if(SelectedLinks, [this](uint64 LinkID) { return Algo::FindIf(Wires, [LinkID](const FWire& Wire) { return Wire.ID == LinkID; }) == Wires.end(); });
        }

        bLinkDragRejected = false;
        bHasDrawn = true;
    }

    void FGraphEditor::EndWindow()
    {
        ImGui::EndChild();
    }

    bool FGraphEditor::FindPinAnchor(uint32 PinID, ImVec2& OutAnchor) const
    {
        auto Iterator = Pins.find(PinID);
        if (Iterator == Pins.end())
        {
            return false;
        }
        OutAnchor = Iterator->second.Anchor;
        return true;
    }

    void FGraphEditor::BuildWires()
    {
        WirePoints.clear();

        // Every tie a wire passes through is a stop on its route.
        THashMap<uint64, TVector<int32>> WiresOfTie;
        {
            THashMap<uint64, int32> WireOfLink;
            for (int32 Index = 0; Index < (int32)Wires.size(); ++Index)
            {
                WireOfLink[Wires[Index].ID] = Index;
            }
            for (const auto& [TieID, Tie] : Ties)
            {
                for (uint64 LinkID : Tie.Links)
                {
                    if (auto Wire = WireOfLink.find(LinkID); Wire != WireOfLink.end())
                    {
                        WiresOfTie[TieID].push_back(Wire->second);
                    }
                }
            }
        }

        // Lanes follow the order the wires arrive in, so a bundle never crosses itself.
        auto ArrivalHeight = [this](const FWire& Wire)
        {
            ImVec2 Start, End;
            FindPinAnchor(Wire.OutputPin, Start);
            FindPinAnchor(Wire.InputPin, End);
            return Start.y + End.y;
        };

        struct FStop
        {
            const FZipTieState* Tie;
            float               Lane;
        };
        TVector<TVector<FStop>> Stops(Wires.size());
        for (auto& [TieID, Group] : WiresOfTie)
        {
            Algo::Sort(Group, [&](int32 A, int32 B) { return ArrivalHeight(Wires[A]) < ArrivalHeight(Wires[B]); });

            float Widest = 0.0f;
            for (int32 Index : Group)
            {
                Widest = Math::Max(Widest, Wires[Index].Thickness);
            }
            const float Spacing = Widest + kTieLaneGap;
            const float Count   = (float)Group.size();
            FZipTieState& Tie = Ties[TieID];
            Tie.HalfSpan = (Count - 1.0f) * Spacing * 0.5f + Widest * 0.5f + kTiePadding;
            for (size_t Index = 0; Index < Group.size(); ++Index)
            {
                Stops[Group[Index]].push_back({ &Tie, ((float)Index - (Count - 1.0f) * 0.5f) * Spacing });
            }
        }

        const float Zoom = Canvas.GetZoom();
        TVector<ImVec2> SettledCenters;
        TVector<ImVec2> Centers;
        TVector<ImVec2> From;
        TVector<ImVec2> To;
        for (int32 WireIndex = 0; WireIndex < (int32)Wires.size(); ++WireIndex)
        {
            FWire& Wire = Wires[WireIndex];
            Wire.PointCount = 0;
            ImVec2 Start, End;
            if ((Wire.Color & IM_COL32_A_MASK) == 0 || !FindPinAnchor(Wire.OutputPin, Start) || !FindPinAnchor(Wire.InputPin, End))
            {
                continue;
            }

            const int32 Segments = Math::Clamp((int32)(Math::Sqrt(ImLengthSqr(End - Start)) * Zoom / 12.0f), 12, 48);
            Wire.FirstPoint = (int32)WirePoints.size();

            // Stops run in order from the output to the input, whichever way the wire points.
            TVector<FStop>& Route = Stops[WireIndex];
            const ImVec2 Direction = End - Start;
            Algo::Sort(Route, [&](const FStop& A, const FStop& B) { return ImDot(A.Tie->Position - Start, Direction) < ImDot(B.Tie->Position - Start, Direction); });

            // A tie still pulling tight or letting go blends from the route without it to the route with it.
            SettledCenters.clear();
            Centers.clear();
            float Blend = 1.0f;
            for (const FStop& Stop : Route)
            {
                if (Stop.Tie->Amount <= 0.0f)
                {
                    continue;
                }
                const ImVec2 Center(Stop.Tie->Position.x, Stop.Tie->Position.y + Stop.Lane);
                Centers.push_back(Center);
                if (Stop.Tie->Amount >= 1.0f)
                {
                    SettledCenters.push_back(Center);
                }
                else
                {
                    Blend = Math::Min(Blend, EaseOutCubic(Stop.Tie->Amount));
                }
            }

            ScratchTiedPath.clear();
            AppendTiedRoute(ScratchTiedPath, Start, End, Centers, Math::Max(Segments, 24 * (int32)Centers.size()));
            if (SettledCenters.size() == Centers.size())
            {
                WirePoints.insert(WirePoints.end(), ScratchTiedPath.begin(), ScratchTiedPath.end());
            }
            else
            {
                const int32 Count = (int32)ScratchTiedPath.size();
                ScratchFreePath.clear();
                AppendTiedRoute(ScratchFreePath, Start, End, SettledCenters, Math::Max(Segments, 24 * (int32)SettledCenters.size()));
                ResampleByLength(ScratchFreePath, Count, From);
                ResampleByLength(ScratchTiedPath, Count, To);
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    WirePoints.push_back(From[Index] + (To[Index] - From[Index]) * Blend);
                }
            }

            Wire.PointCount = (int32)WirePoints.size() - Wire.FirstPoint;
            Wire.Bounds = BoundsOf(&WirePoints[Wire.FirstPoint], Wire.PointCount);
        }
    }

    void FGraphEditor::DrawWires()
    {
        const float  Zoom          = Canvas.GetZoom();
        const float  MinThickness  = 1.25f / Zoom;
        const float  Glow          = 4.0f / Zoom;
        const ImU32  SelectedColor = EditorColors::U32(EditorColors::Accent());
        const ImU32  StrokeColor   = EditorColors::U32(EditorColors::WithAlpha(GetStrokeColor(StrokeKind), 0.6f));
        const ImRect Visible       = Canvas.GetVisibleRect();
        const float  FlowOffset    = Math::Mod((float)ImGui::GetTime() * kFlowSpeed, kFlowSpacing);

        auto TouchesHighlightedNode = [this](uint32 PinID)
        {
            auto Pin = Pins.find(PinID);
            return Pin != Pins.end() && (IsNodeSelected(Pin->second.NodeID) || HoveredNode == Pin->second.NodeID);
        };

        for (const FWire& Wire : Wires)
        {
            if (Wire.PointCount < 2 || !Wire.Bounds.Overlaps(Visible))
            {
                continue;
            }

            const ImVec2* Points    = &WirePoints[Wire.FirstPoint];
            const float   Thickness = Math::Max(Wire.Thickness, MinThickness);
            const bool    bSelected = IsLinkSelected(Wire.ID);
            const bool    bHovered  = HoveredLink == Wire.ID;

            if (bSelected)
            {
                DrawList->AddPolyline(Points, Wire.PointCount, ScaleAlpha(SelectedColor, 0.85f), Thickness + Glow);
            }
            else if (bHovered || TouchesHighlightedNode(Wire.OutputPin) || TouchesHighlightedNode(Wire.InputPin))
            {
                DrawList->AddPolyline(Points, Wire.PointCount, ScaleAlpha(Wire.Color, 0.35f), Thickness + Glow);
            }

            if (Action == EAction::Stroke && Algo::FindIf(StrokeHits, [&Wire](const FGraphStrokeHit& Hit) { return Hit.LinkID == Wire.ID; }) != StrokeHits.end())
            {
                DrawList->AddPolyline(Points, Wire.PointCount, StrokeColor, Thickness + Glow);
            }

            if (Action == EAction::DragItems && Wire.ID == Splice.LinkID && Splice.LinkID == AcceptedSpliceLink)
            {
                DrawList->AddPolyline(Points, Wire.PointCount, EditorColors::U32(EditorColors::WithAlpha(EditorColors::Success(), 0.75f)), Thickness + Glow * 1.5f);
            }

            const bool  bBeingMoved = Action == EAction::DragLink && LinkDrag.bMoveLinks && (Wire.OutputPin == LinkDrag.FromPin || Wire.InputPin == LinkDrag.FromPin);
            const ImU32 Color       = bHovered ? Brighten(Wire.Color, 0.2f) : Wire.Color;
            DrawList->AddPolyline(Points, Wire.PointCount, bBeingMoved ? ScaleAlpha(Color, 0.25f) : Color, Thickness);

            if (!Wire.Badge.empty() && Zoom >= kBadgeMinZoom)
            {
                const float  FontSize = ImGui::GetFontSize() * kBadgeFontScale;
                const ImVec2 TextSize = ImGui::GetFont()->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Wire.Badge.c_str());
                const ImVec2 Padding(4.0f, 1.5f);
                const ImVec2 Middle = Points[Wire.PointCount / 2];
                const ImVec2 Min    = Middle - TextSize * 0.5f - Padding;
                const ImVec2 Max    = Middle + TextSize * 0.5f + Padding;
                DrawList->AddRectFilled(Min, Max, IM_COL32(20, 21, 24, 235), 4.0f);
                DrawList->AddRect(Min, Max, Wire.BadgeColor, 4.0f, 1.0f);
                DrawList->AddText(nullptr, FontSize, Min + Padding, Wire.BadgeColor, Wire.Badge.c_str());
            }

            if (!Wire.bFlow)
            {
                continue;
            }

            // Dots ride the wire from output to input at a steady speed, however the wire is bent.
            const ImU32 DotColor  = Brighten(Wire.Color, 0.35f);
            const float DotRadius = Math::Max(Thickness * 1.1f, 2.5f / Zoom);
            float Walked  = 0.0f;
            float NextDot = FlowOffset;
            for (int32 Index = 1; Index < Wire.PointCount; ++Index)
            {
                const ImVec2 From   = Points[Index - 1];
                const ImVec2 To     = Points[Index];
                const float  Length = Math::Sqrt(ImLengthSqr(To - From));
                while (Length > 0.0f && NextDot <= Walked + Length)
                {
                    DrawList->AddCircleFilled(From + (To - From) * ((NextDot - Walked) / Length), DotRadius, DotColor, 8);
                    NextDot += kFlowSpacing;
                }
                Walked += Length;
            }
        }
    }

    ImRect FGraphEditor::GetZipTieRect(const FZipTieState& State) const
    {
        const float HalfWidth = kTieHeadWidth * 0.5f;
        return ImRect(ImVec2(State.Position.x - HalfWidth, State.Position.y - State.HalfSpan - kTieHeadHeight),
                      ImVec2(State.Position.x + HalfWidth, State.Position.y + State.HalfSpan));
    }

    void FGraphEditor::DrawZipTies()
    {
        const float Zoom   = Canvas.GetZoom();
        const ImU32 Strap  = IM_COL32(44, 46, 52, 255);
        const ImU32 Ridge  = IM_COL32(28, 29, 33, 255);
        const ImU32 Shine  = IM_COL32(255, 255, 255, 40);
        const ImU32 Shadow = IM_COL32(0, 0, 0, 90);

        for (const auto& [TieID, Tie] : Ties)
        {
            if (Tie.Amount <= 0.0f || Tie.HalfSpan <= 0.0f)
            {
                continue;
            }

            const float Fade  = Math::Saturate(Tie.Amount * 3.0f);
            const float Cinch = EaseOutBack(Tie.Amount);
            const float Half  = Math::Lerp(Math::Max(Tie.HalfSpan * 2.4f, Tie.HalfSpan + 14.0f), Tie.HalfSpan, Cinch);
            const float Tail  = EaseOutCubic(Math::Saturate((Tie.Amount - 0.3f) / 0.7f));

            const ImVec2 Center = Tie.Position;
            const ImVec2 BandMin(Center.x - kTieBandWidth * 0.5f, Center.y - Half);
            const ImVec2 BandMax(Center.x + kTieBandWidth * 0.5f, Center.y + Half);
            const ImVec2 HeadMin(Center.x - kTieHeadWidth * 0.5f, BandMin.y - kTieHeadHeight);
            const ImVec2 HeadMax(Center.x + kTieHeadWidth * 0.5f, BandMin.y + 1.0f);
            const ImVec2 Lift(1.0f, 1.5f);

            DrawList->AddRectFilled(BandMin + Lift, BandMax + Lift, ScaleAlpha(Shadow, Fade), kTieBandWidth * 0.5f);
            DrawList->AddRectFilled(HeadMin + Lift, HeadMax + Lift, ScaleAlpha(Shadow, Fade), 2.5f);

            DrawList->AddRectFilled(BandMin, BandMax, ScaleAlpha(Strap, Fade), kTieBandWidth * 0.5f);
            for (float RidgeY = BandMin.y + 3.0f; RidgeY < BandMax.y - 2.0f; RidgeY += 2.5f)
            {
                DrawList->AddLine(ImVec2(BandMin.x + 1.5f, RidgeY), ImVec2(BandMax.x - 1.5f, RidgeY), ScaleAlpha(Ridge, Fade), 1.0f / Zoom);
            }
            DrawList->AddLine(ImVec2(BandMin.x + 1.2f, BandMin.y + 2.0f), ImVec2(BandMin.x + 1.2f, BandMax.y - 2.0f), ScaleAlpha(Shine, Fade), 1.0f);

            // The strap's loose end feeds out of the head as it is pulled tight.
            if (Tail > 0.0f)
            {
                const ImVec2 Root(HeadMax.x - 1.0f, HeadMin.y + 3.0f);
                const ImVec2 Tip = Root + ImVec2(14.0f, -8.0f) * Tail;
                DrawList->AddBezierQuadratic(Root, Root + ImVec2(8.0f, 0.0f) * Tail, Tip, ScaleAlpha(Strap, Fade), 4.5f);
                DrawList->AddCircleFilled(Tip, 2.25f, ScaleAlpha(Strap, Fade));
            }

            DrawList->AddRectFilled(HeadMin, HeadMax, ScaleAlpha(Strap, Fade), 2.5f);
            DrawList->AddRectFilled(ImVec2(Center.x - 3.0f, HeadMin.y + 2.5f), ImVec2(Center.x + 3.0f, HeadMax.y - 3.0f), ScaleAlpha(Ridge, Fade), 1.0f);
            DrawList->AddLine(ImVec2(HeadMin.x + 2.0f, HeadMin.y + 1.2f), ImVec2(HeadMax.x - 2.0f, HeadMin.y + 1.2f), ScaleAlpha(Shine, Fade), 1.0f);

            const bool bSelected = IsZipTieSelected(TieID);
            const bool bHovered  = HoveredTie == TieID;
            if (bSelected || bHovered || Tie.Flash > 0.0f)
            {
                ImVec4 Color = EditorColors::WithAlpha(EditorColors::Accent(), bSelected ? 1.0f : 0.55f);
                if (!bSelected && !bHovered)
                {
                    Color = ImVec4(1.0f, 1.0f, 1.0f, Tie.Flash * 0.8f);
                }
                const ImVec2 Grow(2.0f, 2.0f);
                DrawList->AddRect(HeadMin - Grow, ImVec2(HeadMax.x, BandMax.y) + Grow, EditorColors::U32(Color), 4.0f, 1.5f / Zoom);
            }
        }
    }

    void FGraphEditor::DrawOverlay()
    {
        const float  Zoom  = Canvas.GetZoom();
        const ImVec2 Mouse = ImGui::GetIO().MousePos;

        if (Action == EAction::DragLink)
        {
            auto From = Pins.find(LinkDrag.FromPin);
            if (From != Pins.end())
            {
                ImVec2 Target = Mouse;
                const bool bOverTarget = LinkDrag.ToPin != 0 || LinkDrag.ToNode != 0;
                if (LinkDrag.ToPin != 0)
                {
                    FindPinAnchor(LinkDrag.ToPin, Target);
                }

                ImU32 Color = From->second.Color;
                if (bOverTarget)
                {
                    Color = bLinkDragRejected ? EditorColors::U32(EditorColors::Danger()) : Brighten(Color, 0.15f);
                }

                auto DrawDragWire = [this, Color, Zoom](ImVec2 Start, ImVec2 End)
                {
                    const float Bend = WireBend(Start, End);
                    DrawList->AddBezierCubic(Start, ImVec2(Start.x + Bend, Start.y), ImVec2(End.x - Bend, End.y), End, Color, Math::Max(2.5f, 1.5f / Zoom));
                };

                // Moving wires shows each one already leaving from the cursor, so it is clear what will land where.
                if (LinkDrag.bMoveLinks)
                {
                    for (const FWire& Wire : Wires)
                    {
                        ImVec2 FarEnd;
                        if (Wire.OutputPin == LinkDrag.FromPin && FindPinAnchor(Wire.InputPin, FarEnd))
                        {
                            DrawDragWire(Target, FarEnd);
                        }
                        else if (Wire.InputPin == LinkDrag.FromPin && FindPinAnchor(Wire.OutputPin, FarEnd))
                        {
                            DrawDragWire(FarEnd, Target);
                        }
                    }
                }
                else if (From->second.Kind == EGraphPinKind::Output)
                {
                    DrawDragWire(From->second.Anchor, Target);
                }
                else
                {
                    DrawDragWire(Target, From->second.Anchor);
                }
                DrawList->AddCircleFilled(Target, 3.5f, Color);

                if (LinkDrag.ToPin != 0)
                {
                    const ImVec4 Ring = bLinkDragRejected ? EditorColors::Danger() : EditorColors::Success();
                    DrawList->AddCircle(Target, 8.0f, EditorColors::U32(Ring), 0, 2.0f / Zoom);
                }
            }
        }

        if (Action == EAction::BoxSelect)
        {
            const ImVec2 Min = ImMin(Press.CanvasPos, Mouse);
            const ImVec2 Max = ImMax(Press.CanvasPos, Mouse);
            DrawList->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.08f)));
            DrawList->AddRect(Min, Max, EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.7f)), 0.0f, 1.0f / Zoom);
        }

        if (Action == EAction::Stroke && !StrokePoints.empty())
        {
            const ImU32 Color = EditorColors::U32(GetStrokeColor(StrokeKind));
            StrokePoints.push_back(Mouse);
            DrawList->AddPolyline(StrokePoints.data(), (int32)StrokePoints.size(), Color, 2.0f / Zoom);
            StrokePoints.pop_back();

            // Sized against the zoom, so the label reads the same however far out the canvas is.
            DrawList->AddText(nullptr, ImGui::GetFontSize() / Zoom, Mouse + ImVec2(12.0f, 10.0f) / Zoom, Color, GetStrokeLabel(StrokeKind));
        }
    }

    uint8 FGraphEditor::GetCommentResizeEdges(const FNodeState& State, ImVec2 Mouse) const
    {
        const float  Slop = kCommentEdgeSlop / Canvas.GetZoom();
        const ImRect Rect = State.GetRect();
        ImRect Outer = Rect;
        Outer.Expand(Slop);
        if (!Outer.Contains(Mouse))
        {
            return 0;
        }

        const float Grip = kCommentCornerGrip / Canvas.GetZoom();
        if (Mouse.x > Rect.Max.x - Grip && Mouse.y > Rect.Max.y - Grip)
        {
            return kResizeRight | kResizeBottom;
        }

        uint8 Edges = 0;
        Edges |= Mouse.x < Rect.Min.x + Slop ? kResizeLeft : (Mouse.x > Rect.Max.x - Slop ? kResizeRight : 0);
        Edges |= Mouse.y < Rect.Min.y + Slop ? kResizeTop : (Mouse.y > Rect.Max.y - Slop ? kResizeBottom : 0);
        return Edges;
    }

    void FGraphEditor::HitTest(ImVec2 Mouse)
    {
        HoveredItem        = EGraphItem::Background;
        HoveredNode        = 0;
        HoveredPin         = 0;
        HoveredLink        = 0;
        HoveredTie         = 0;
        HoveredResizeEdges = 0;

        int64 TopNode = 0;
        int32 TopRank = -1;
        for (const auto& [NodeID, State] : Nodes)
        {
            if (State.LastFrame == Frame && !State.bComment && State.Rank > TopRank && State.GetRect().Contains(Mouse))
            {
                TopRank = State.Rank;
                TopNode = NodeID;
            }
        }

        // A pin wins over its own node, but not over a node drawn above it.
        int32 PinRank = -1;
        for (const auto& [PinID, Pin] : Pins)
        {
            auto Owner = Nodes.find(Pin.NodeID);
            const int32 Rank = Owner != Nodes.end() ? Owner->second.Rank : 0;
            if (Rank >= TopRank && Rank > PinRank && Pin.Rect.Contains(Mouse))
            {
                PinRank = Rank;
                HoveredPin = PinID;
            }
        }
        if (HoveredPin != 0)
        {
            HoveredItem = EGraphItem::Pin;
            return;
        }

        if (TopNode != 0)
        {
            HoveredItem = EGraphItem::Node;
            HoveredNode = TopNode;
            return;
        }

        for (const auto& [TieID, Tie] : Ties)
        {
            ImRect Rect = GetZipTieRect(Tie);
            Rect.Expand(2.0f / Canvas.GetZoom());
            if (Tie.LastFrame == Frame && Rect.Contains(Mouse))
            {
                HoveredItem = EGraphItem::ZipTie;
                HoveredTie  = TieID;
                return;
            }
        }

        if (ReportedHoveredLink != 0)
        {
            HoveredItem = EGraphItem::Link;
            HoveredLink = ReportedHoveredLink;
            return;
        }

        HoveredLink = FindWireAt(Mouse, kWireHitSlop, 0);
        if (HoveredLink != 0)
        {
            HoveredItem = EGraphItem::Link;
            return;
        }

        for (int32 Index = (int32)FrameComments.size() - 1; Index >= 0; --Index)
        {
            const FNodeState& State = Nodes[FrameComments[Index]];
            const uint8  Edges = GetCommentResizeEdges(State, Mouse);
            const ImRect Header(State.Position, ImVec2(State.Position.x + State.Size.x, State.Position.y + State.HeaderHeight));
            if (Edges != 0 || Header.Contains(Mouse))
            {
                HoveredItem        = EGraphItem::Node;
                HoveredNode        = FrameComments[Index];
                HoveredResizeEdges = Edges;
                return;
            }
        }
    }

    void FGraphEditor::ProcessInput(ImVec2 Mouse)
    {
        const ImGuiIO& IO = ImGui::GetIO();
        const ImVec2 ScreenMouse = Canvas.GetScreenMousePos();
        if (Action == EAction::Minimap)
        {
            return;
        }

        if (Action == EAction::None && bCanvasHovered && !bMouseOverMinimap)
        {
            for (ImGuiMouseButton Button : { ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle })
            {
                if (!ImGui::IsMouseClicked(Button))
                {
                    continue;
                }

                Press             = FPress();
                Press.Item        = HoveredItem;
                Press.Button      = Button;
                Press.ScreenPos   = ScreenMouse;
                Press.CanvasPos   = Mouse;
                Press.bCtrl       = IO.KeyCtrl;
                Press.bShift      = IO.KeyShift;
                Press.bAlt        = IO.KeyAlt;
                Press.ResizeEdges = HoveredResizeEdges;
                switch (HoveredItem)
                {
                    case EGraphItem::Node:   Press.ID = (uint64)HoveredNode; break;
                    case EGraphItem::Pin:    Press.ID = HoveredPin;          break;
                    case EGraphItem::Link:   Press.ID = HoveredLink;         break;
                    case EGraphItem::ZipTie: Press.ID = HoveredTie;          break;
                    default:                                                 break;
                }
                Action = EAction::Pending;

                if (Button == ImGuiMouseButton_Right && HoveredItem == EGraphItem::Node && !IsNodeSelected(HoveredNode))
                {
                    SelectNode(HoveredNode, false);
                }

                if (Button != ImGuiMouseButton_Left)
                {
                    break;
                }

                const bool bDoubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                switch (HoveredItem)
                {
                    case EGraphItem::Pin:
                    {
                        if (Press.bAlt)
                        {
                            BreakLinksPin = HoveredPin;
                            Action = EAction::None;
                        }
                        break;
                    }
                    case EGraphItem::Node:
                    case EGraphItem::ZipTie:
                    {
                        const bool bNode     = HoveredItem == EGraphItem::Node;
                        const bool bSelected = bNode ? IsNodeSelected(HoveredNode) : IsZipTieSelected(HoveredTie);
                        if (bNode && bDoubleClick && Press.ResizeEdges == 0)
                        {
                            DoubleClickedNode = HoveredNode;
                        }

                        if (Press.bCtrl && bSelected)
                        {
                            if (bNode)
                            {
                                DeselectNode(HoveredNode);
                            }
                            else
                            {
                                SelectedTies.erase(HoveredTie);
                            }
                        }
                        else if (!bSelected)
                        {
                            if (bNode)
                            {
                                SelectNode(HoveredNode, Press.bCtrl || Press.bShift);
                            }
                            else
                            {
                                SelectZipTie(HoveredTie, Press.bCtrl || Press.bShift);
                            }
                        }
                        else if (!Press.bShift)
                        {
                            Press.bSelectAloneOnClick = true;
                        }

                        if (bNode && !Nodes[HoveredNode].bComment)
                        {
                            Nodes[HoveredNode].ZOrder = NextZOrder++;
                        }
                        break;
                    }
                    case EGraphItem::Link:
                    {
                        if (bDoubleClick)
                        {
                            DoubleClickedLink = HoveredLink;
                        }
                        break;
                    }
                    case EGraphItem::Background:
                    {
                        if (!Press.bCtrl && !Press.bShift)
                        {
                            ClearSelection();
                        }
                        bBackgroundClicked = true;
                        BackgroundClickPos = Mouse;
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
        }

        if (Action == EAction::None)
        {
            return;
        }

        if (Action == EAction::Pending && ImGui::IsMouseDown(Press.Button) && ImLengthSqr(ScreenMouse - Press.ScreenPos) > kDragThreshold * kDragThreshold)
        {
            StartDrag(Mouse);
        }

        switch (Action)
        {
            case EAction::BoxSelect:
            {
                UpdateBoxSelection(Mouse);
                break;
            }
            case EAction::DragItems:
            {
                UpdateSplice(Mouse);
                break;
            }
            case EAction::DragLink:
            {
                LinkDrag.ToPin  = HoveredPin != LinkDrag.FromPin ? HoveredPin : 0;
                LinkDrag.ToNode = LinkDrag.ToPin == 0 && HoveredItem == EGraphItem::Node ? HoveredNode : 0;
                break;
            }
            case EAction::Stroke:
            {
                if (ImLengthSqr(Canvas.CanvasToScreen(Mouse) - Canvas.CanvasToScreen(StrokePoints.back())) > 9.0f)
                {
                    StrokePoints.push_back(Mouse);
                }
                UpdateStrokeHits();
                break;
            }
            default:
                break;
        }

        if (!ImGui::IsMouseDown(Press.Button))
        {
            FinishAction();
        }
    }

    void FGraphEditor::StartDrag(ImVec2 Mouse)
    {
        if (Press.Button == ImGuiMouseButton_Left)
        {
            switch (Press.Item)
            {
                case EGraphItem::Pin:
                {
                    Action           = EAction::DragLink;
                    LinkDrag         = FGraphLinkDrag();
                    LinkDrag.FromPin = (uint32)Press.ID;
                    bLinkDragActive  = true;
                    LinkDrag.bMoveLinks = Press.bCtrl && Algo::AnyOf(Wires, [this](const FWire& Wire)
                    {
                        return Wire.OutputPin == LinkDrag.FromPin || Wire.InputPin == LinkDrag.FromPin;
                    });
                    return;
                }
                case EGraphItem::Node:
                {
                    const FNodeState& State = Nodes[(int64)Press.ID];
                    if (Press.ResizeEdges != 0 && State.bComment)
                    {
                        Action              = EAction::ResizeComment;
                        ResizeStartPosition = State.Position;
                        ResizeStartSize     = ImVec2(State.Size.x, State.Size.y - State.HeaderHeight);
                        DragDelta           = ImVec2(0.0f, 0.0f);
                        return;
                    }
                    CollectDragSet();
                    Action = EAction::DragItems;
                    return;
                }
                case EGraphItem::ZipTie:
                {
                    CollectDragSet();
                    Action = EAction::DragItems;
                    return;
                }
                default:
                {
                    BoxBaseSelection = Press.bCtrl || Press.bShift ? SelectedNodes : THashSet<int64>();
                    Action = EAction::BoxSelect;
                    UpdateBoxSelection(Mouse);
                    return;
                }
            }
        }

        if (Press.Button == ImGuiMouseButton_Right && (Press.bCtrl || Press.bShift || Press.bAlt))
        {
            StrokeKind = Press.bCtrl ? EGraphStroke::Cut : (Press.bShift ? EGraphStroke::Reroute : EGraphStroke::ZipTie);
            StrokePoints.clear();
            StrokePoints.push_back(Press.CanvasPos);
            StrokeHits.clear();
            Action = EAction::Stroke;
            return;
        }

        Action = EAction::Pan;
    }

    void FGraphEditor::CollectDragSet()
    {
        DragStartNodes.clear();
        DragStartTies.clear();
        DragDelta = ImVec2(0.0f, 0.0f);

        const bool bPressedNode     = Press.Item == EGraphItem::Node;
        const bool bPressedSelected = bPressedNode ? IsNodeSelected((int64)Press.ID) : IsZipTieSelected(Press.ID);
        if (bPressedSelected)
        {
            for (int64 NodeID : SelectedNodes)
            {
                DragStartNodes[NodeID] = Nodes[NodeID].Position;
            }
            for (uint64 TieID : SelectedTies)
            {
                DragStartTies[TieID] = Ties[TieID].Position;
            }
        }
        else if (bPressedNode)
        {
            DragStartNodes[(int64)Press.ID] = Nodes[(int64)Press.ID].Position;
        }
        else
        {
            DragStartTies[Press.ID] = Ties[Press.ID].Position;
        }

        // A comment carries whatever sits inside it, nested comments and ties included.
        TVector<ImRect> Carriers;
        for (const auto& [NodeID, Start] : DragStartNodes)
        {
            const FNodeState& State = Nodes[NodeID];
            if (State.bComment)
            {
                Carriers.push_back(State.GetRect());
            }
        }
        for (const ImRect& Carrier : Carriers)
        {
            for (const auto& [NodeID, State] : Nodes)
            {
                if (State.LastFrame == Frame && Carrier.Contains(State.GetRect()))
                {
                    DragStartNodes.try_emplace(NodeID, State.Position);
                }
            }
            for (const auto& [TieID, Tie] : Ties)
            {
                if (Tie.LastFrame == Frame && Carrier.Contains(Tie.Position))
                {
                    DragStartTies.try_emplace(TieID, Tie.Position);
                }
            }
        }
    }

    void FGraphEditor::UpdateBoxSelection(ImVec2 Mouse)
    {
        const ImRect Box(ImMin(Press.CanvasPos, Mouse), ImMax(Press.CanvasPos, Mouse));
        SelectedNodes = BoxBaseSelection;
        for (const auto& [NodeID, State] : Nodes)
        {
            const ImRect Rect = State.GetRect();
            if (State.bComment ? Box.Contains(Rect) : Box.Overlaps(Rect))
            {
                SelectedNodes.insert(NodeID);
            }
        }

        if (!Press.bCtrl && !Press.bShift)
        {
            SelectedLinks.clear();
            SelectedTies.clear();
        }
        for (const auto& [TieID, Tie] : Ties)
        {
            if (Tie.LastFrame == Frame && Box.Contains(Tie.Position))
            {
                SelectedTies.insert(TieID);
            }
        }
    }

    void FGraphEditor::UpdateStrokeHits()
    {
        StrokeHits.clear();

        StrokePoints.push_back(ImGui::GetIO().MousePos);
        const ImRect StrokeBounds = BoundsOf(StrokePoints.data(), (int32)StrokePoints.size());
        for (const FWire& Wire : Wires)
        {
            if (Wire.PointCount < 2 || !Wire.Bounds.Overlaps(StrokeBounds))
            {
                continue;
            }

            const ImVec2* Points = &WirePoints[Wire.FirstPoint];
            bool bCrossed = false;
            for (int32 StrokeIndex = 1; StrokeIndex < (int32)StrokePoints.size() && !bCrossed; ++StrokeIndex)
            {
                for (int32 WireIndex = 1; WireIndex < Wire.PointCount && !bCrossed; ++WireIndex)
                {
                    ImVec2 Crossing;
                    if (SegmentsCross(StrokePoints[StrokeIndex - 1], StrokePoints[StrokeIndex], Points[WireIndex - 1], Points[WireIndex], Crossing))
                    {
                        StrokeHits.push_back(FGraphStrokeHit{ Wire.ID, Crossing });
                        bCrossed = true;
                    }
                }
            }
        }
        StrokePoints.pop_back();
    }

    uint64 FGraphEditor::FindWireAt(ImVec2 Point, float ScreenSlop, int64 IgnoredNode) const
    {
        auto OwnedByIgnored = [this, IgnoredNode](uint32 PinID)
        {
            auto Pin = Pins.find(PinID);
            return IgnoredNode != 0 && Pin != Pins.end() && Pin->second.NodeID == IgnoredNode;
        };

        uint64 Found = 0;
        float  BestDistance = FLT_MAX;
        for (const FWire& Wire : Wires)
        {
            const float Reach = Wire.Thickness * 0.5f + ScreenSlop / Canvas.GetZoom();
            ImRect Bounds = Wire.Bounds;
            Bounds.Expand(Reach);
            if (Wire.PointCount < 2 || !Bounds.Contains(Point) || OwnedByIgnored(Wire.OutputPin) || OwnedByIgnored(Wire.InputPin))
            {
                continue;
            }

            const ImVec2* Points = &WirePoints[Wire.FirstPoint];
            for (int32 Index = 1; Index < Wire.PointCount; ++Index)
            {
                const float Distance = ImLengthSqr(Point - ImLineClosestPoint(Points[Index - 1], Points[Index], Point));
                if (Distance <= Reach * Reach && Distance < BestDistance)
                {
                    BestDistance = Distance;
                    Found        = Wire.ID;
                }
            }
        }
        return Found;
    }

    void FGraphEditor::UpdateSplice(ImVec2 Mouse)
    {
        Splice = FGraphSplice();
        if (DragStartNodes.size() != 1 || !DragStartTies.empty())
        {
            return;
        }

        const int64 NodeID = DragStartNodes.begin()->first;
        auto State = Nodes.find(NodeID);
        if (State == Nodes.end() || State->second.bComment)
        {
            return;
        }

        Splice.LinkID = FindWireAt(Mouse, kSpliceSlop, NodeID);
        Splice.NodeID = Splice.LinkID != 0 ? NodeID : 0;
    }

    bool FGraphEditor::UpdateMinimap(ImVec2 ScreenMouse)
    {
        ImRect Content(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
        for (const auto& [NodeID, State] : Nodes)
        {
            if (State.LastFrame == Frame)
            {
                Content.Add(State.GetRect());
            }
        }

        // Only worth the corner while something is out of view.
        const ImRect Visible = Canvas.GetVisibleRect();
        bMinimapVisible = Content.Min.x <= Content.Max.x && (Action == EAction::Minimap || !Visible.Contains(Content));
        if (!bMinimapVisible)
        {
            return false;
        }

        const ImRect Screen = Canvas.GetScreenRect();
        const float  Em     = ImGui::GetFontSize();
        const ImVec2 Margin(kMinimapMargin, kMinimapMargin);
        MinimapRect = ImRect(Screen.Max - ImVec2(kMinimapWidthEm, kMinimapHeightEm) * Em - Margin, Screen.Max - Margin);

        MinimapContent = Content;
        MinimapContent.Add(Visible);
        const ImVec2 Inner  = MinimapRect.GetSize() - ImVec2(kMinimapPadding, kMinimapPadding) * 2.0f;
        const ImVec2 Extent = ImMax(MinimapContent.GetSize(), ImVec2(1.0f, 1.0f));
        MinimapScale  = Math::Min(Inner.x / Extent.x, Inner.y / Extent.y);
        MinimapOrigin = MinimapRect.Min + ImVec2(kMinimapPadding, kMinimapPadding) + (Inner - Extent * MinimapScale) * 0.5f;

        const bool bOver = MinimapRect.Contains(ScreenMouse);
        if (Action == EAction::None && bOver && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            Action = EAction::Minimap;
        }

        if (Action == EAction::Minimap)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                NavigateToView(MinimapContent.Min + (ScreenMouse - MinimapOrigin) / MinimapScale, Canvas.GetZoom(), 0.0f);
            }
            else
            {
                Action = EAction::None;
            }
        }
        return bOver || Action == EAction::Minimap;
    }

    void FGraphEditor::DrawMinimap()
    {
        if (!bMinimapVisible)
        {
            return;
        }

        auto ToMap = [this](ImVec2 Point) { return MinimapOrigin + (Point - MinimapContent.Min) * MinimapScale; };
        const ImU32 Accent = EditorColors::U32(EditorColors::Accent());

        ImDrawList* Overlay = ImGui::GetWindowDrawList();
        Overlay->AddRectFilled(MinimapRect.Min, MinimapRect.Max, IM_COL32(16, 17, 20, 225), 6.0f);
        Overlay->AddRect(MinimapRect.Min, MinimapRect.Max, IM_COL32(255, 255, 255, 30), 6.0f);
        Overlay->PushClipRect(MinimapRect.Min, MinimapRect.Max, true);

        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            for (const auto& [NodeID, State] : Nodes)
            {
                if (State.LastFrame != Frame || State.bComment != (Pass == 0))
                {
                    continue;
                }

                const ImU32 Color = State.bComment ? IM_COL32(255, 255, 255, 16) : (IsNodeSelected(NodeID) ? Accent : IM_COL32(165, 170, 180, 150));
                Overlay->AddRectFilled(ToMap(State.Position), ToMap(State.Position + State.Size), Color, 1.5f);
            }
        }

        const ImRect Visible = Canvas.GetVisibleRect();
        Overlay->AddRectFilled(ToMap(Visible.Min), ToMap(Visible.Max), ScaleAlpha(Accent, 0.1f), 2.0f);
        Overlay->AddRect(ToMap(Visible.Min), ToMap(Visible.Max), ScaleAlpha(Accent, 0.85f), 2.0f, 1.5f);
        Overlay->PopClipRect();
    }

    void FGraphEditor::FinishAction()
    {
        switch (Action)
        {
            case EAction::Pending:
            {
                if (Press.Button == ImGuiMouseButton_Right)
                {
                    ContextRequest = FGraphContextRequest{ Press.Item, Press.ID };
                    break;
                }
                if (Press.Button != ImGuiMouseButton_Left)
                {
                    break;
                }

                if (Press.bSelectAloneOnClick && Press.Item == EGraphItem::Node)
                {
                    SelectNode((int64)Press.ID, false);
                }
                else if (Press.bSelectAloneOnClick && Press.Item == EGraphItem::ZipTie)
                {
                    SelectZipTie(Press.ID, false);
                }
                else if (Press.Item == EGraphItem::Link)
                {
                    if (Press.bCtrl && IsLinkSelected(Press.ID))
                    {
                        SelectedLinks.erase(Press.ID);
                    }
                    else
                    {
                        SelectLink(Press.ID, Press.bCtrl || Press.bShift);
                    }
                }
                break;
            }
            case EAction::DragItems:
            {
                if (Splice.LinkID != 0 && Splice.LinkID == AcceptedSpliceLink)
                {
                    SpliceDrop = Splice;
                }
                break;
            }
            case EAction::DragLink:
            {
                LinkDrag.bDropped = true;
                break;
            }
            case EAction::Stroke:
            {
                FinishedStroke = StrokeKind;
                break;
            }
            default:
                break;
        }

        Action = EAction::None;
        Splice = FGraphSplice();
        AcceptedSpliceLink = 0;
        DragStartNodes.clear();
        DragStartTies.clear();
        DragDelta = ImVec2(0.0f, 0.0f);
        StrokePoints.clear();
    }

    void FGraphEditor::CancelAction()
    {
        switch (Action)
        {
            case EAction::BoxSelect:
            {
                SelectedNodes = BoxBaseSelection;
                break;
            }
            case EAction::DragLink:
            {
                bLinkDragActive = false;
                LinkDrag = FGraphLinkDrag();
                break;
            }
            case EAction::Stroke:
            {
                StrokeHits.clear();
                StrokePoints.clear();
                break;
            }
            case EAction::Pending:
                break;
            default:
                return;
        }
        Action = EAction::None;
    }

    EGraphStroke FGraphEditor::GetFinishedStroke(TVector<FGraphStrokeHit>& OutHits) const
    {
        OutHits = FinishedStroke != EGraphStroke::None ? StrokeHits : TVector<FGraphStrokeHit>();
        return FinishedStroke;
    }

    void FGraphEditor::NavigateTo(const ImRect& CanvasRect, float Duration)
    {
        bNavigationPending  = true;
        NavigationTarget    = CanvasRect;
        NavigationDuration  = Duration;
        NavigationExactZoom = 0.0f;
    }

    void FGraphEditor::NavigateToView(ImVec2 Center, float Zoom, float Duration)
    {
        bNavigationPending  = true;
        NavigationTarget    = ImRect(Center, Center);
        NavigationDuration  = Duration;
        NavigationExactZoom = Zoom;
    }

    void FGraphEditor::NavigateToContent(float Duration)
    {
        ImRect Bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
        for (const auto& [NodeID, State] : Nodes)
        {
            Bounds.Add(State.GetRect());
        }
        if (Bounds.Min.x <= Bounds.Max.x)
        {
            Bounds.Expand(40.0f);
            NavigateTo(Bounds, Duration);
        }
    }

    void FGraphEditor::NavigateToSelection(float Duration)
    {
        ImRect Bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
        for (int64 NodeID : SelectedNodes)
        {
            if (auto State = Nodes.find(NodeID); State != Nodes.end())
            {
                Bounds.Add(State->second.GetRect());
            }
        }
        for (uint64 TieID : SelectedTies)
        {
            if (auto Tie = Ties.find(TieID); Tie != Ties.end())
            {
                Bounds.Add(GetZipTieRect(Tie->second));
            }
        }
        if (Bounds.Min.x <= Bounds.Max.x)
        {
            Bounds.Expand(40.0f);
            NavigateTo(Bounds, Duration);
        }
    }

    void FGraphEditor::ClearSelection()
    {
        SelectedNodes.clear();
        SelectedLinks.clear();
        SelectedTies.clear();
    }

    void FGraphEditor::SelectNode(int64 NodeID, bool bAppend)
    {
        if (!bAppend)
        {
            ClearSelection();
        }
        SelectedNodes.insert(NodeID);
    }

    void FGraphEditor::DeselectNode(int64 NodeID)
    {
        SelectedNodes.erase(NodeID);
    }

    TVector<int64> FGraphEditor::GetSelectedNodes() const
    {
        return TVector<int64>(SelectedNodes.begin(), SelectedNodes.end());
    }

    void FGraphEditor::SelectLink(uint64 LinkID, bool bAppend)
    {
        if (!bAppend)
        {
            ClearSelection();
        }
        SelectedLinks.insert(LinkID);
    }

    TVector<uint64> FGraphEditor::GetSelectedLinks() const
    {
        return TVector<uint64>(SelectedLinks.begin(), SelectedLinks.end());
    }

    void FGraphEditor::SelectZipTie(uint64 TieID, bool bAppend)
    {
        if (!bAppend)
        {
            ClearSelection();
        }
        SelectedTies.insert(TieID);
    }

    TVector<uint64> FGraphEditor::GetSelectedZipTies() const
    {
        return TVector<uint64>(SelectedTies.begin(), SelectedTies.end());
    }
}
