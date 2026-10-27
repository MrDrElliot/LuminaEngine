#pragma once

#include "GraphCanvas.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    enum class EGraphPinKind : uint8
    {
        Input,
        Output,
    };

    // Read by shape as well as color, so pin types stay apart for anyone who cannot tell the colors.
    enum class EGraphPinShape : uint8
    {
        Circle,
        Diamond,
        Triangle,
        Square,
        Hexagon,
    };

    // How a pin answers the wire being pulled right now.
    enum class EGraphPinHighlight : uint8
    {
        None,
        Accepts,
        Refuses,
    };

    struct FGraphPinIcon
    {
        ImU32              Color      = IM_COL32_WHITE;
        ImU32              InnerColor = 0;
        EGraphPinShape     Shape      = EGraphPinShape::Circle;
        EGraphPinHighlight Highlight  = EGraphPinHighlight::None;
        bool               bConnected = false;
    };

    enum class EGraphItem : uint8
    {
        None,
        Background,
        Node,
        Pin,
        Link,
        ZipTie,
    };

    // A right-drag across wires with a modifier held, applied to every wire the stroke crosses.
    enum class EGraphStroke : uint8
    {
        None,
        ZipTie,
        Reroute,
        Cut,
    };

    enum class EGraphShortcut : uint8
    {
        Delete    = 1 << 0,
        Copy      = 1 << 1,
        Cut       = 1 << 2,
        Paste     = 1 << 3,
        Duplicate = 1 << 4,
    };

    struct FGraphNodeStyle
    {
        ImVec4 Padding             = ImVec4(8.0f, 4.0f, 8.0f, 8.0f);
        float  Rounding            = 8.0f;
        float  BorderWidth         = 1.0f;
        float  HoveredBorderWidth  = 2.0f;
        float  SelectedBorderWidth = 2.5f;
        ImU32  Background          = IM_COL32(29, 31, 35, 245);
        ImU32  Border              = IM_COL32(255, 255, 255, 30);
    };

    struct FGraphContextRequest
    {
        EGraphItem Item = EGraphItem::None;
        uint64     ID   = 0;
    };

    struct FGraphLinkDrag
    {
        uint32 FromPin    = 0;
        uint32 ToPin      = 0;
        int64  ToNode     = 0;
        bool   bDropped   = false;
        // Ctrl held at the press, so the pin's existing wires move to the target instead of a new one starting.
        bool   bMoveLinks = false;
    };

    // A lone node dragged over a wire, which the graph may splice in between that wire's ends.
    struct FGraphSplice
    {
        int64  NodeID = 0;
        uint64 LinkID = 0;
    };

    struct FGraphStrokeHit
    {
        uint64 LinkID = 0;
        ImVec2 Point;
    };

    // Immediate mode node canvas. Everything is submitted between Begin and End, and what the user did is read back after End.
    class EDITOR_API FGraphEditor
    {
    public:

        FGraphEditor() = default;
        ~FGraphEditor() = default;
        FGraphEditor(const FGraphEditor&) = delete;
        FGraphEditor& operator=(const FGraphEditor&) = delete;
        FGraphEditor(FGraphEditor&&) = delete;
        FGraphEditor& operator=(FGraphEditor&&) = delete;

        // Opens a child window filling the rest of the current one. NodeCount bounds how many nodes and comments are submitted before End.
        void Begin(const char* ID, int32 NodeCount);

        // Leaves the canvas but stays in its window, so tooltips, popups and screen-space overlays can follow before EndWindow.
        void End();
        void EndWindow();

        void BeginNode(int64 NodeID, ImVec2& InOutPosition, const FGraphNodeStyle& Style = FGraphNodeStyle());
        void EndNode();

        // A box behind every node, dragged by its header with everything inside it and resized from its edges.
        void Comment(int64 NodeID, ImVec2& InOutPosition, ImVec2& InOutBodySize, float HeaderHeight, ImVec2 MinBodySize);

        // A pin hits on the content between these, and its wires meet the edge facing them.
        void BeginPin(uint32 PinID, EGraphPinKind Kind, ImU32 Color);
        void EndPin();

        // A pin laid out by hand, with no content of its own.
        void Pin(uint32 PinID, EGraphPinKind Kind, ImU32 Color, const ImRect& HitRect, ImVec2 Anchor);

        // A fully transparent wire is neither drawn nor hit, for a graph that draws its own and reports hover through SetHoveredLink.
        void Link(uint64 LinkID, uint32 OutputPin, uint32 InputPin, ImU32 Color, float Thickness, bool bFlow = false);

        // A short label riding the middle of the wire submitted last.
        void LinkBadge(FStringView Text, ImU32 Color);

        // Bundles these wires through Position, so they run side by side across it.
        void ZipTie(uint64 TieID, ImVec2& InOutPosition, const TVector<uint64>& LinkIDs);

        // Drawn under the node's widgets. Valid from that node's BeginNode or Comment until End.
        ImDrawList* GetNodeBackgroundDrawList(int64 NodeID);

        ImVec2 ScreenToCanvas(ImVec2 Point) const { return Canvas.ScreenToCanvas(Point); }
        ImVec2 CanvasToScreen(ImVec2 Point) const { return Canvas.CanvasToScreen(Point); }
        float  GetZoom() const { return Canvas.GetZoom(); }
        ImRect GetVisibleCanvasRect() const { return Canvas.GetVisibleRect(); }
        bool   HasDrawn() const { return bHasDrawn; }
        bool   GetPinAnchor(uint32 PinID, ImVec2& OutAnchor) const { return FindPinAnchor(PinID, OutAnchor); }
        void   GetView(ImVec2& OutCenter, float& OutZoom) const;

        // Zero until the node has been drawn once.
        ImVec2 GetNodeSize(int64 NodeID) const;

        // Fitting never magnifies past 1, so framing a single node does not blow it up.
        void NavigateTo(const ImRect& CanvasRect, float Duration);
        void NavigateToContent(float Duration);
        void NavigateToSelection(float Duration);

        // Lands on exactly this zoom, unlike the fitting calls above.
        void NavigateToView(ImVec2 Center, float Zoom, float Duration);

        void  ClearSelection();
        void  SelectNode(int64 NodeID, bool bAppend);
        void  DeselectNode(int64 NodeID);
        bool  IsNodeSelected(int64 NodeID) const { return SelectedNodes.find(NodeID) != SelectedNodes.end(); }
        int32 GetSelectedNodeCount() const { return (int32)SelectedNodes.size(); }
        TVector<int64> GetSelectedNodes() const;

        void SelectLink(uint64 LinkID, bool bAppend);
        bool IsLinkSelected(uint64 LinkID) const { return SelectedLinks.find(LinkID) != SelectedLinks.end(); }
        TVector<uint64> GetSelectedLinks() const;

        void SelectZipTie(uint64 TieID, bool bAppend);
        bool IsZipTieSelected(uint64 TieID) const { return SelectedTies.find(TieID) != SelectedTies.end(); }
        TVector<uint64> GetSelectedZipTies() const;

        bool HasSelection() const { return !SelectedNodes.empty() || !SelectedLinks.empty() || !SelectedTies.empty(); }

        int64  GetHoveredNode() const { return HoveredNode; }
        uint32 GetHoveredPin() const { return HoveredPin; }
        uint64 GetHoveredLink() const { return HoveredLink; }
        uint64 GetHoveredZipTie() const { return HoveredTie; }

        // For wires the graph draws itself, so they select, delete and open menus like any other.
        void SetHoveredLink(uint64 LinkID) { ReportedHoveredLink = LinkID; }

        const FGraphContextRequest& GetContextRequest() const { return ContextRequest; }
        int64  GetDoubleClickedNode() const { return DoubleClickedNode; }
        uint64 GetDoubleClickedLink() const { return DoubleClickedLink; }

        // The pin Alt-clicked this frame, whose wires the user wants gone.
        uint32 GetBreakLinksPin() const { return BreakLinksPin; }

        // A left press on empty canvas this frame.
        bool GetBackgroundClick(ImVec2& OutCanvasPos) const { OutCanvasPos = BackgroundClickPos; return bBackgroundClicked; }

        bool WasShortcutPressed(EGraphShortcut Shortcut) const { return (Shortcuts & (uint8)Shortcut) != 0; }

        const FGraphLinkDrag* GetLinkDrag() const { return bLinkDragActive ? &LinkDrag : nullptr; }

        // The wire a lone dragged node is over right now, which the graph answers through AcceptSplice for the next frame.
        const FGraphSplice* GetSpliceCandidate() const { return Splice.LinkID != 0 ? &Splice : nullptr; }
        void AcceptSplice(bool bAccept) { AcceptedSpliceLink = bAccept ? Splice.LinkID : 0; }

        // The accepted splice the user dropped this frame.
        const FGraphSplice* GetSpliceDrop() const { return SpliceDrop.LinkID != 0 ? &SpliceDrop : nullptr; }

        // The wire being pulled draws red over its current target.
        void RejectLinkDrag() { bLinkDragRejected = true; }

        // The stroke finished this frame and the wires it crossed.
        EGraphStroke GetFinishedStroke(TVector<FGraphStrokeHit>& OutHits) const;

        // A node cannot open a tooltip window from inside the canvas, so it hands the text here to show after End.
        void SetTooltip(FStringView Text) { Tooltip.assign(Text.data(), Text.size()); }
        const FString& GetTooltip() const { return Tooltip; }

    private:

        enum class EAction : uint8
        {
            None,
            Pending,
            Pan,
            BoxSelect,
            DragItems,
            ResizeComment,
            DragLink,
            Stroke,
            Minimap,
        };

        struct FNodeState
        {
            ImVec2          Position;
            ImVec2          Size;
            ImVec2          MinBodySize;
            FGraphNodeStyle Style;
            uint32          ZOrder       = 0;
            int32           LastFrame    = -1;
            int32           Rank         = 0;
            float           HeaderHeight = 0.0f;
            bool            bComment     = false;

            ImRect GetRect() const { return ImRect(Position, ImVec2(Position.x + Size.x, Position.y + Size.y)); }
        };

        struct FPinState
        {
            int64         NodeID = 0;
            ImRect        Rect;
            ImVec2        Anchor;
            EGraphPinKind Kind  = EGraphPinKind::Input;
            ImU32         Color = 0;
        };

        struct FWire
        {
            uint64 ID         = 0;
            uint32 OutputPin  = 0;
            uint32 InputPin   = 0;
            ImU32  Color      = 0;
            float  Thickness  = 1.0f;
            bool   bFlow      = false;
            int32  FirstPoint = 0;
            int32  PointCount = 0;
            ImRect Bounds;
            FString Badge;
            ImU32   BadgeColor = 0;
        };

        struct FZipTieState
        {
            ImVec2          Position;
            TVector<uint64> Links;
            float           Amount    = 0.0f;
            float           Flash     = 0.0f;
            float           HalfSpan  = 0.0f;
            int32           LastFrame = -1;
        };

        struct FPress
        {
            EGraphItem       Item   = EGraphItem::None;
            uint64           ID     = 0;
            ImGuiMouseButton Button = ImGuiMouseButton_Left;
            ImVec2           ScreenPos;
            ImVec2           CanvasPos;
            bool             bCtrl  = false;
            bool             bShift = false;
            bool             bAlt   = false;
            uint8            ResizeEdges = 0;
            bool             bSelectAloneOnClick = false;
        };

        void UpdateView();
        void UpdateShortcuts();
        void AssignRanks();
        void SetChannel(int32 Channel) { Splitter.SetCurrentChannel(DrawList, Channel); }
        int32 GetNodeChannel(const FNodeState& State, bool bBackground) const;

        void BuildWires();
        void DrawWires();
        void DrawZipTies();
        void DrawOverlay();
        void HitTest(ImVec2 Mouse);
        void ProcessInput(ImVec2 Mouse);
        void StartDrag(ImVec2 Mouse);
        void FinishAction();
        void CancelAction();
        void UpdateBoxSelection(ImVec2 Mouse);
        void UpdateStrokeHits();
        void CollectDragSet();
        void UpdateSplice(ImVec2 Mouse);
        bool UpdateMinimap(ImVec2 ScreenMouse);
        void DrawMinimap();
        uint64 FindWireAt(ImVec2 Point, float ScreenSlop, int64 IgnoredNode) const;

        bool   FindPinAnchor(uint32 PinID, ImVec2& OutAnchor) const;
        uint8  GetCommentResizeEdges(const FNodeState& State, ImVec2 Mouse) const;
        ImRect GetZipTieRect(const FZipTieState& State) const;

        FGraphCanvas                     Canvas;
        ImDrawListSplitter               Splitter;
        ImDrawList*                      DrawList = nullptr;
        int32                            Frame = 0;
        int32                            RankCount = 0;
        int32                            NextRank = 0;
        uint32                           NextZOrder = 1;
        bool                             bHasDrawn = false;
        bool                             bCanvasHovered = false;

        THashMap<int64, FNodeState>      Nodes;
        TVector<int64>                   FrameComments;
        int64                            CurrentNode = 0;
        int64                            TopNodeUnderMouse = 0;
        ImVec2                           HiddenMousePos;
        bool                             bMouseHiddenFromNode = false;

        THashMap<uint32, FPinState>      Pins;
        uint32                           CurrentPin = 0;
        FPinState                        CurrentPinState;

        TVector<FWire>                   Wires;
        TVector<ImVec2>                  WirePoints;
        TVector<ImVec2>                  ScratchFreePath;
        TVector<ImVec2>                  ScratchTiedPath;
        THashMap<uint64, FZipTieState>   Ties;

        THashSet<int64>                  SelectedNodes;
        THashSet<uint64>                 SelectedLinks;
        THashSet<uint64>                 SelectedTies;

        EGraphItem                       HoveredItem = EGraphItem::None;
        int64                            HoveredNode = 0;
        uint32                           HoveredPin  = 0;
        uint64                           HoveredLink = 0;
        uint64                           HoveredTie  = 0;
        uint8                            HoveredResizeEdges = 0;
        uint64                           ReportedHoveredLink = 0;

        FGraphContextRequest             ContextRequest;
        int64                            DoubleClickedNode = 0;
        uint64                           DoubleClickedLink = 0;
        uint32                           BreakLinksPin = 0;
        bool                             bBackgroundClicked = false;
        ImVec2                           BackgroundClickPos;
        uint8                            Shortcuts = 0;
        FString                          Tooltip;

        EAction                          Action = EAction::None;
        FPress                           Press;
        THashMap<int64, ImVec2>          DragStartNodes;
        THashMap<uint64, ImVec2>         DragStartTies;
        ImVec2                           DragDelta;
        ImVec2                           ResizeStartPosition;
        ImVec2                           ResizeStartSize;
        THashSet<int64>                  BoxBaseSelection;
        FGraphLinkDrag                   LinkDrag;
        bool                             bLinkDragActive = false;
        bool                             bLinkDragRejected = false;
        EGraphStroke                     StrokeKind = EGraphStroke::None;
        EGraphStroke                     FinishedStroke = EGraphStroke::None;
        TVector<ImVec2>                  StrokePoints;
        TVector<FGraphStrokeHit>         StrokeHits;
        FGraphSplice                     Splice;
        FGraphSplice                     SpliceDrop;
        uint64                           AcceptedSpliceLink = 0;
        ImRect                           MinimapRect;
        ImRect                           MinimapContent;
        float                            MinimapScale = 0.0f;
        ImVec2                           MinimapOrigin;
        bool                             bMinimapVisible = false;
        bool                             bMouseOverMinimap = false;

        bool                             bNavigationPending = false;
        ImRect                           NavigationTarget;
        float                            NavigationDuration = 0.0f;
        bool                             bNavigating = false;
        float                            NavigationTime = 0.0f;
        ImVec2                           NavigationFromCenter;
        ImVec2                           NavigationToCenter;
        float                            NavigationFromZoom = 1.0f;
        float                            NavigationToZoom = 1.0f;
        float                            NavigationExactZoom = 0.0f;
        float                            TargetZoom = 1.0f;
        bool                             bZooming = false;
        ImVec2                           ZoomAnchorScreen;
        ImVec2                           ZoomAnchorCanvas;
    };
}
