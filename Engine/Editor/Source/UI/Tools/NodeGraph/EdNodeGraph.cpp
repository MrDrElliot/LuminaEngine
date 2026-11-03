#include "EdNodeGraph.h"

#include "EdGraphNode.h"
#include "EdNode_Comment.h"
#include "EdNode_Reroute.h"
#include "GraphAlgorithms.h"
#include "GraphNodeRegistry.h"
#include "Core/Object/Class.h"
#include <Core/Reflection/Type/LuminaTypes.h>
#include "GraphNodeBuilder.h"
#include "imgui_internal.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"
#include "Core/Templates/IntegerCompare.h"

namespace Lumina
{
    namespace
    {
        // Raw pointers deliberately, since a TStrongObjectPtr clipboard would outlive its graph at teardown.
        TVector<CEdGraphNode*>& GetNodeClipboard()
        {
            static TVector<CEdGraphNode*> Clipboard;
            return Clipboard;
        }

        // Canvas-space center of the copied set, so a paste lands relative to the cursor.
        ImVec2 GClipboardPivot(0.0f, 0.0f);

        void ForgetClipboardNode(CEdGraphNode* Node)
        {
            TVector<CEdGraphNode*>& Clipboard = GetNodeClipboard();
            Clipboard.erase(Algo::Remove(Clipboard, Node), Clipboard.end());
        }

        const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* FindLink(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links, uint64 LinkID)
        {
            if (LinkID == 0)
            {
                return nullptr;
            }
            auto Link = Algo::FindIf(Links, [LinkID](const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>& Pair) { return CEdNodeGraph::MakeLinkID(Pair.first, Pair.second) == LinkID; });
            return Link != Links.end() ? &*Link : nullptr;
        }

        // Tied wires are stored as output and input pin id pairs, the same halves a link id packs.
        uint64 GetTiedLinkID(const SGraphZipTie& Tie, size_t Pair)
        {
            return ((uint64)Tie.Links[Pair * 2] << 32) | (uint64)Tie.Links[Pair * 2 + 1];
        }

        template<typename TPredicate>
        bool RetainTiedLinks(SGraphZipTie& Tie, TPredicate Keep)
        {
            TVector<uint32> Kept;
            for (size_t Pair = 0; Pair < Tie.Links.size() / 2; ++Pair)
            {
                if (Keep(GetTiedLinkID(Tie, Pair)))
                {
                    Kept.push_back(Tie.Links[Pair * 2]);
                    Kept.push_back(Tie.Links[Pair * 2 + 1]);
                }
            }
            const bool bChanged = Kept.size() != Tie.Links.size();
            Tie.Links = Move(Kept);
            return bChanged;
        }
    }

    // FullName becomes the emitted shader variable, so anything outside the identifier set collapses.
    static FString SanitizeNodeIdentifier(const FString& In)
    {
        FString Out = In;
        for (char& Char : Out)
        {
            const bool bLegal = (Char >= 'A' && Char <= 'Z')
                             || (Char >= 'a' && Char <= 'z')
                             || (Char >= '0' && Char <= '9')
                             || Char == '_';

            if (!bLegal)
            {
                Char = '_';
            }
        }

        return Out;
    }

    static constexpr uint32 GNodeErrorHeaderColor   = IM_COL32(200, 45, 45, 255);
    static constexpr uint32 GNodeWarningHeaderColor = IM_COL32(205, 155, 35, 255);
    static constexpr uint32 GNodeErrorPinColor      = IM_COL32(255, 70, 70, 255);
    static constexpr uint32 GNodeErrorBubbleColor   = IM_COL32(58, 18, 18, 240);
    static constexpr uint32 GNodeErrorTextColor     = IM_COL32(255, 220, 220, 255);

    static constexpr float  GRerouteKnotRadius     = 6.0f;
    static constexpr float  GDefaultWireThickness  = 2.5f;

    CEdNodeGraph::CEdNodeGraph()
    {
    }

    CEdNodeGraph::~CEdNodeGraph()
    {
    }

    
    

    void CEdNodeGraph::Initialize()
    {
    }

    void CEdNodeGraph::Shutdown()
    {
        // Otherwise closing the editor you copied from leaves a paste pointing at freed nodes.
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid())
            {
                ForgetClipboardNode(Node.Get());
            }
        }

        // The tool owning that history is going away with the canvas.
        TransactionManager = nullptr;
        UndoImage = FNodeGraphImage();
    }

    void CEdNodeGraph::Serialize(FArchive& Ar)
    {
        Super::Serialize(Ar);
    }

    void CEdNodeGraph::PostLoad()
    {
        Super::PostLoad();
        
        TVector<TStrongObjectPtr<CEdGraphNode>> SavedNodes = Move(Nodes);
        TVector<uint32> SavedConnections = Move(Connections);
        Nodes.clear();
        Connections.clear();

        // Reconciling before the links are back would drop everything keyed off them.
        bIsPostLoading = true;

        for (const TStrongObjectPtr<CEdGraphNode>& Node : SavedNodes)
        {
            if (Node.IsValid())
            {
                AddNode(Node.Get());
            }
        }

        Connections = Move(SavedConnections);
        RelinkFromConnections();

        // The load's one reconcile, rebuilding Connections and re-matching link-keyed data against them.
        bIsPostLoading = false;
        ValidateGraph();
    }

    void CEdNodeGraph::RelinkFromConnections()
    {
        for (size_t i = 0; i + 1 < Connections.size(); i += 2)
        {
            uint32 InputID = Connections[i];
            uint32 OutputID = Connections[i + 1];

            CEdNodeGraphPin* InputPin = nullptr;
            CEdNodeGraphPin* OutputPin = nullptr;

            for (const auto& NodeRef : Nodes)
            {
                CEdGraphNode* Node = NodeRef.Get();
                if (!InputPin)
                {
                    InputPin = Node->GetPin(InputID, ENodePinDirection::Input);
                }
                if (!OutputPin)
                {
                    OutputPin = Node->GetPin(OutputID, ENodePinDirection::Output);
                }
                if (InputPin && OutputPin)
                {
                    break;
                }
            }

            if (!InputPin || !OutputPin || InputPin->OwningNode == OutputPin->OwningNode)
            {
                continue;
            }

            if (InputPin->HasConnection() && !GetSchema().AllowsMultipleConnections(InputPin))
            {
                continue;
            }

            OutputPin->AddConnection(InputPin);
            InputPin->AddConnection(OutputPin);
        }
    }

    CPackage* CEdNodeGraph::GetNodeOuter()
    {
        return GetPackage();
    }

    void CEdNodeGraph::DrawPinDebugValue(CEdNodeGraphPin* Pin)
    {
        if (!DebugContext.bEnabled || DebugContext.PinValues == nullptr || Pin == nullptr)
        {
            return;
        }

        auto It = DebugContext.PinValues->find(Pin);
        if (It == DebugContext.PinValues->end())
        {
            return;
        }

        ImGui::Spring(0);
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 1.0f, 1.0f), "%s", It->second.c_str());
    }

    namespace
    {
        constexpr float kCommentHeaderHeight = 34.0f;
        constexpr float kCommentRounding     = 8.0f;
        constexpr float kCommentPadding      = 12.0f;
        constexpr float kCommentMinWidth     = 160.0f;
        constexpr float kCommentMinHeight    = 60.0f;
        constexpr float kCommentWrapMargin   = 32.0f;

        ImU32 ScaleCommentAlpha(ImU32 Color, float Scale)
        {
            const uint32 Alpha = (uint32)Math::Clamp((float)((Color >> IM_COL32_A_SHIFT) & 0xFF) * Scale, 0.0f, 255.0f);
            return (Color & ~IM_COL32_A_MASK) | (Alpha << IM_COL32_A_SHIFT);
        }

        // Breaks at spaces and at the text's own newlines, and splits a word only when it is wider than a whole line.
        void WrapWords(ImFont* Font, float FontSize, const FString& Text, float MaxWidth, TVector<FString>& OutLines)
        {
            OutLines.clear();
            auto Measure = [Font, FontSize](const FString& Line)
            {
                return Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Line.c_str(), Line.c_str() + Line.size()).x;
            };

            size_t ParagraphStart = 0;
            while (ParagraphStart <= Text.size())
            {
                size_t ParagraphEnd = Text.find('\n', ParagraphStart);
                if (ParagraphEnd == FString::npos)
                {
                    ParagraphEnd = Text.size();
                }

                FString Line;
                size_t WordStart = ParagraphStart;
                while (WordStart < ParagraphEnd)
                {
                    size_t WordEnd = Text.find(' ', WordStart);
                    if (WordEnd == FString::npos || WordEnd > ParagraphEnd)
                    {
                        WordEnd = ParagraphEnd;
                    }
                    const FString Word = Text.substr(WordStart, WordEnd - WordStart);
                    const FString Candidate = Line.empty() ? Word : Line + " " + Word;
                    if (Line.empty() || Measure(Candidate) <= MaxWidth)
                    {
                        Line = Candidate;
                    }
                    else
                    {
                        OutLines.push_back(Line);
                        Line = Word;
                    }
                    WordStart = WordEnd + 1;
                }
                OutLines.push_back(Line);
                ParagraphStart = ParagraphEnd + 1;
            }
        }

        // Cuts a line that does not fit at a whole character and ends it with an ellipsis.
        void AddTextEllipsized(ImDrawList* DrawList, ImFont* Font, float FontSize, ImVec2 Position, ImU32 Color, const FString& Text, float MaxWidth)
        {
            const char* Begin = Text.c_str();
            const char* End   = Begin + Text.size();
            if (Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Begin, End).x <= MaxWidth)
            {
                DrawList->AddText(Font, FontSize, Position, Color, Begin, End);
                return;
            }

            constexpr const char* Ellipsis = "...";
            const float EllipsisWidth = Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Ellipsis).x;
            while (End > Begin && Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Begin, End).x + EllipsisWidth > MaxWidth)
            {
                do
                {
                    --End;
                }
                while (End > Begin && (*End & 0xC0) == 0x80);
            }

            FString Shown(Begin, (size_t)(End - Begin));
            Shown += Ellipsis;
            DrawList->AddText(Font, FontSize, Position, Color, Shown.c_str(), Shown.c_str() + Shown.size());
        }
    }

    void CEdNodeGraph::DrawRerouteNode(CEdGraphNode* Node)
    {
        // The knot itself moves the node, and a lobe either side of it takes or gives a wire.
        constexpr float LobeReach = GRerouteKnotRadius * 2.5f;

        CEdNodeGraphPin* InputPin  = Node->GetInputPins().empty() ? nullptr : Node->GetInputPins()[0].Get();
        CEdNodeGraphPin* OutputPin = Node->GetOutputPins().empty() ? nullptr : Node->GetOutputPins()[0].Get();
        const ImU32 Color = OutputPin != nullptr ? GetWireColor(OutputPin) : IM_COL32_WHITE;

        FGraphNodeStyle Style;
        Style.Padding             = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        Style.Rounding            = GRerouteKnotRadius;
        Style.BorderWidth         = 0.0f;
        Style.HoveredBorderWidth  = 1.5f;
        Style.SelectedBorderWidth = 2.0f;
        Style.Background          = 0;

        BeginCanvasNode(Node, Style);
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(GRerouteKnotRadius * 2.0f, GRerouteKnotRadius * 2.0f));
        const ImVec2 Center = Min + ImVec2(GRerouteKnotRadius, GRerouteKnotRadius);

        if (InputPin != nullptr)
        {
            Editor.Pin(InputPin->GetPinGUID(), EGraphPinKind::Input, Color,
                ImRect(ImVec2(Center.x - LobeReach, Center.y - GRerouteKnotRadius), ImVec2(Center.x - GRerouteKnotRadius * 0.6f, Center.y + GRerouteKnotRadius)), Center);
        }
        if (OutputPin != nullptr)
        {
            Editor.Pin(OutputPin->GetPinGUID(), EGraphPinKind::Output, Color,
                ImRect(ImVec2(Center.x + GRerouteKnotRadius * 0.6f, Center.y - GRerouteKnotRadius), ImVec2(Center.x + LobeReach, Center.y + GRerouteKnotRadius)), Center);
        }
        Editor.EndNode();

        if (ImDrawList* DrawList = Editor.GetNodeBackgroundDrawList(Node->GetNodeID()))
        {
            DrawList->AddCircleFilled(Center, GRerouteKnotRadius + 1.5f, ImGui::GetColorU32(IM_COL32(0, 0, 0, 180)), 16);
            DrawList->AddCircleFilled(Center, GRerouteKnotRadius, ImGui::GetColorU32(Color), 16);
            DrawList->AddCircleFilled(Center, GRerouteKnotRadius * 0.4f, ImGui::GetColorU32(IM_COL32(0, 0, 0, 110)), 12);
        }
    }

    void CEdNodeGraph::DrawNodeErrorBubble(const CEdGraphNode* Node)
    {
        const int64  NodeID   = Node->GetNodeID();
        const ImVec2 Position(Node->GetNodeX(), Node->GetNodeY());
        const ImVec2 NodeSize = Editor.GetNodeSize(NodeID);
        ImDrawList*  DrawList = Editor.GetNodeBackgroundDrawList(NodeID);
        if (DrawList == nullptr)
        {
            return;
        }

        constexpr float kGap      = 8.0f;
        constexpr float kPadding  = 8.0f;
        constexpr float kMinWidth = 260.0f;
        constexpr float kRounding = 6.0f;
        constexpr float kArrowX   = 18.0f;

        const EdNodeGraph::FError& Error = Node->GetError();
        const FString Title = FString(LE_ICON_EXCLAMATION_THICK " ") + Error.Name;

        ImFont*     Font      = ImGui::GetFont();
        const float FontSize  = ImGui::GetFontSize();
        const float Width     = Math::Max(NodeSize.x, kMinWidth);
        const float WrapWidth = Width - kPadding * 2.0f;
        const float TitleHeight = Font->CalcTextSizeA(FontSize, FLT_MAX, WrapWidth, Title.c_str()).y;
        const float BodyHeight  = Error.Description.empty() ? 0.0f : Font->CalcTextSizeA(FontSize, FLT_MAX, WrapWidth, Error.Description.c_str()).y + kPadding * 0.5f;

        const ImVec2 Min(Position.x, Position.y + NodeSize.y + kGap);
        const ImVec2 Max(Min.x + Width, Min.y + kPadding * 2.0f + TitleHeight + BodyHeight);

        DrawList->AddTriangleFilled(ImVec2(Min.x + kArrowX, Min.y), ImVec2(Min.x + kArrowX + kGap, Min.y - kGap), ImVec2(Min.x + kArrowX + kGap * 2.0f, Min.y), GNodeErrorHeaderColor);
        DrawList->AddRectFilled(Min, Max, GNodeErrorBubbleColor, kRounding);
        DrawList->AddRect(Min, Max, GNodeErrorHeaderColor, kRounding, 1.5f);

        const ImVec2 TextPos = Min + ImVec2(kPadding, kPadding);
        DrawList->AddText(Font, FontSize, TextPos, GNodeErrorPinColor, Title.c_str(), nullptr, WrapWidth);
        if (!Error.Description.empty())
        {
            DrawList->AddText(Font, FontSize, TextPos + ImVec2(0.0f, TitleHeight + kPadding * 0.5f), GNodeErrorTextColor, Error.Description.c_str(), nullptr, WrapWidth);
        }
    }

    CEdGraphNode* CEdNodeGraph::InsertRerouteOnLink(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin, ImVec2 CanvasPos)
    {
        if (OutputPin == nullptr || InputPin == nullptr)
        {
            return nullptr;
        }

        CClass* RerouteClass = GetRerouteNodeClass();
        if (RerouteClass == nullptr)
        {
            return nullptr;
        }

        CEdGraphNode* RerouteNode = CreateNode(RerouteClass);
        if (RerouteNode == nullptr)
        {
            return nullptr;
        }

        CEdNodeGraphPin* RerouteIn  = RerouteNode->GetInputPins().empty()  ? nullptr : RerouteNode->GetInputPins()[0].Get();
        CEdNodeGraphPin* RerouteOut = RerouteNode->GetOutputPins().empty() ? nullptr : RerouteNode->GetOutputPins()[0].Get();
        if (RerouteIn == nullptr || RerouteOut == nullptr)
        {
            return RerouteNode;
        }

        // Break the original wire and rebuild it as Source -> RerouteIn, RerouteOut -> Target.
        OutputPin->DisconnectFrom(InputPin);

        OutputPin->AddConnection(RerouteIn);
        RerouteIn->AddConnection(OutputPin);

        RerouteOut->AddConnection(InputPin);
        InputPin->AddConnection(RerouteOut);

        RerouteNode->SetGridPos(CanvasPos.x - GRerouteKnotRadius, CanvasPos.y - GRerouteKnotRadius);
        PruneZipTies();
        ValidateGraph();
        return RerouteNode;
    }

    // Matched by name, since a lazily rebuilt pin set would make an index match wire the wrong pin.
    static CEdNodeGraphPin* FindPinByName(CEdGraphNode* Node, const FString& Name, ENodePinDirection Direction)
    {
        if (Node == nullptr)
        {
            return nullptr;
        }

        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
        {
            if (Pin.IsValid() && Pin->GetPinName() == Name)
            {
                return Pin.Get();
            }
        }
        return nullptr;
    }

    // Runs once every clone exists, since a link can point forwards in the list as easily as backwards.
    static void RelinkClones(const TVector<CEdGraphNode*>& SourceOrder, const THashMap<CEdGraphNode*, CEdGraphNode*>& Clones)
    {
        // Walking input pins only visits each link once, so nothing gets connected twice.
        for (CEdGraphNode* Source : SourceOrder)
        {
            const auto CloneItr = Clones.find(Source);
            if (CloneItr == Clones.end())
            {
                continue;
            }

            for (const TStrongObjectPtr<CEdNodeGraphPin>& SourceInput : Source->GetInputPins())
            {
                if (!SourceInput.IsValid())
                {
                    continue;
                }

                for (CEdNodeGraphPin* Upstream : SourceInput->GetConnections())
                {
                    if (Upstream == nullptr)
                    {
                        continue;
                    }

                    // A link reaching outside the set would hang the clone off the original's neighbor.
                    const auto UpstreamItr = Clones.find(Upstream->GetOwningNode());
                    if (UpstreamItr == Clones.end())
                    {
                        continue;
                    }

                    CEdNodeGraphPin* CloneInput  = FindPinByName(CloneItr->second, SourceInput->GetPinName(), ENodePinDirection::Input);
                    CEdNodeGraphPin* CloneOutput = FindPinByName(UpstreamItr->second, Upstream->GetPinName(), ENodePinDirection::Output);
                    if (CloneInput == nullptr || CloneOutput == nullptr)
                    {
                        continue;
                    }

                    CloneOutput->AddConnection(CloneInput);
                    CloneInput->AddConnection(CloneOutput);
                }
            }
        }
    }

    THashMap<CEdGraphNode*, CEdGraphNode*> CEdNodeGraph::CloneNodes(const TVector<CEdGraphNode*>& SourceNodes, ImVec2 Delta)
    {
        // Source -> clone, so the link pass can map an original endpoint onto its copy.
        THashMap<CEdGraphNode*, CEdGraphNode*> Clones;
        Clones.reserve(SourceNodes.size());

        for (CEdGraphNode* Source : SourceNodes)
        {
            if (Source == nullptr)
            {
                continue;
            }

            CEdGraphNode* Clone = CreateNode(Source->GetClass());
            if (Clone == nullptr)
            {
                continue;
            }

            Source->CopyPropertiesTo(Clone);
            Clone->PostCloneFrom(Source);
            Clones.emplace(Source, Clone);

            Clone->SetGridPos(Source->GetNodeX() + Delta.x, Source->GetNodeY() + Delta.y);
            Editor.SelectNode(Clone->GetNodeID(), true);
        }

        RelinkClones(SourceNodes, Clones);

        ValidateGraph();

        // Without this a pasted state pair keeps its wire but loses the transition's conditions and blend.
        for (CEdGraphNode* Source : SourceNodes)
        {
            if (Source != nullptr && Source->GetOwningGraph() != nullptr)
            {
                PostCloneContent(Source->GetOwningGraph(), Clones);
                break;
            }
        }
        return Clones;
    }

    void CEdNodeGraph::CollectSelectedNodesForClone(TVector<CEdGraphNode*>& OutNodes, ImVec2& OutPivot)
    {
        OutNodes.clear();
        OutPivot = ImVec2(0.0f, 0.0f);

        ImRect Bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (Node == nullptr || !Node->IsDeletable() || !Editor.IsNodeSelected(Node->GetNodeID()))
            {
                continue;
            }

            OutNodes.push_back(Node);
            Bounds.Add(GetNodeBounds(Node));
        }

        if (!OutNodes.empty())
        {
            OutPivot = Bounds.GetCenter();
        }
    }

    ImRect CEdNodeGraph::GetNodeBounds(const CEdGraphNode* Node) const
    {
        const ImVec2 Min(Node->GetNodeX(), Node->GetNodeY());
        return ImRect(Min, Min + Editor.GetNodeSize(Node->GetNodeID()));
    }

    void CEdNodeGraph::CloneContentFrom(const CEdNodeGraph* Source)
    {
        if (Source == nullptr)
        {
            return;
        }

        THashMap<CEdGraphNode*, CEdGraphNode*> Clones;
        Clones.reserve(Source->Nodes.size());

        TVector<CEdGraphNode*> SourceOrder;
        SourceOrder.reserve(Source->Nodes.size());

        for (const TStrongObjectPtr<CEdGraphNode>& SourceNode : Source->Nodes)
        {
            if (!SourceNode.IsValid())
            {
                continue;
            }

            CEdGraphNode* Clone = CreateNode(SourceNode->GetClass());
            if (Clone == nullptr)
            {
                continue;
            }

            SourceNode->CopyPropertiesTo(Clone);
            Clone->PostCloneFrom(SourceNode.Get());

            // Positions are DuplicateTransient and the copy has no saved editor layout to key off.
            Clone->SetGridPos(SourceNode->GetNodeX(), SourceNode->GetNodeY());
            Clones.emplace(SourceNode.Get(), Clone);
            SourceOrder.push_back(SourceNode.Get());
        }

        RelinkClones(SourceOrder, Clones);

        ValidateGraph();
        PostCloneContent(Source, Clones);
    }

    uint64 CEdNodeGraph::GetTreeContentVersion() const
    {
        THashSet<const CEdNodeGraph*> Visited;
        return AccumulateTreeContentVersion(Visited);
    }

    uint64 CEdNodeGraph::AccumulateTreeContentVersion(THashSet<const CEdNodeGraph*>& Visited) const
    {
        if (!Visited.insert(this).second)
        {
            return 0;
        }

        uint64 Version = ContentVersion;

        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (!Node.IsValid())
            {
                continue;
            }

            if (const CEdNodeGraph* SubGraph = Node->GetOwnedSubGraph())
            {
                Version += SubGraph->AccumulateTreeContentVersion(Visited);
            }
        }

        return Version;
    }

    uint32 CEdNodeGraph::UnaliasSubGraphs(THashSet<CEdNodeGraph*>& Visited)
    {
        uint32 Repaired = 0;

        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (!Node.IsValid())
            {
                continue;
            }

            CEdNodeGraph* SubGraph = Node->GetOwnedSubGraph();
            if (SubGraph == nullptr)
            {
                continue;
            }

            if (!Visited.insert(SubGraph).second)
            {
                Node->ReplaceSubGraphWithCopy();
                ++Repaired;

                SubGraph = Node->GetOwnedSubGraph();
                if (SubGraph == nullptr || !Visited.insert(SubGraph).second)
                {
                    continue;
                }
            }

            Repaired += SubGraph->UnaliasSubGraphs(Visited);
        }

        return Repaired;
    }

    void CEdNodeGraph::AlignSelectedNodes(ENodeAlignment Alignment)
    {
        struct FEntry
        {
            CEdGraphNode* Node;
            ImVec2        Pos;
            ImVec2        Size;
        };

        TVector<FEntry> Entries;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            // A comment frames other nodes, so lining it up with them would only pull it off what it describes.
            CEdGraphNode* Node = NodeRef.Get();
            if (Node != nullptr && !Node->IsCommentNode() && Editor.IsNodeSelected(Node->GetNodeID()))
            {
                Entries.push_back(FEntry{ Node, ImVec2(Node->GetNodeX(), Node->GetNodeY()), Editor.GetNodeSize(Node->GetNodeID()) });
            }
        }

        const int32 Count        = (int32)Entries.size();
        const bool  bDistributeX = Alignment == ENodeAlignment::DistributeX;
        const bool  bDistributeY = Alignment == ENodeAlignment::DistributeY;
        if (Count < ((bDistributeX || bDistributeY) ? 3 : 2))
        {
            return;
        }

        SetPendingEditLabel("Align Nodes");
        NotifyCosmeticChange();

        float MinLeft = FLT_MAX, MaxRight = -FLT_MAX, MinTop = FLT_MAX, MaxBottom = -FLT_MAX;
        float SumCenterX = 0.0f, SumCenterY = 0.0f, SumWidth = 0.0f, SumHeight = 0.0f;
        for (const FEntry& Entry : Entries)
        {
            MinLeft    = Math::Min(MinLeft,   Entry.Pos.x);
            MaxRight   = Math::Max(MaxRight,  Entry.Pos.x + Entry.Size.x);
            MinTop     = Math::Min(MinTop,    Entry.Pos.y);
            MaxBottom  = Math::Max(MaxBottom, Entry.Pos.y + Entry.Size.y);
            SumCenterX += Entry.Pos.x + Entry.Size.x * 0.5f;
            SumCenterY += Entry.Pos.y + Entry.Size.y * 0.5f;
            SumWidth   += Entry.Size.x;
            SumHeight  += Entry.Size.y;
        }

        const float CenterX = SumCenterX / (float)Count;
        const float CenterY = SumCenterY / (float)Count;

        // Distribute walks in screen order, or nodes get shuffled into whatever order they were clicked.
        if (bDistributeX || bDistributeY)
        {
            Algo::Sort(Entries, [bDistributeX](const FEntry& A, const FEntry& B)
            {
                return bDistributeX ? (A.Pos.x < B.Pos.x) : (A.Pos.y < B.Pos.y);
            });

            const float Span   = bDistributeX ? (MaxRight - MinLeft) : (MaxBottom - MinTop);
            const float Used   = bDistributeX ? SumWidth : SumHeight;
            const float Gap    = (Span - Used) / (float)(Count - 1);
            float       Cursor = bDistributeX ? MinLeft : MinTop;

            for (FEntry& Entry : Entries)
            {
                if (bDistributeX)
                {
                    Entry.Pos.x = Cursor;
                    Cursor += Entry.Size.x + Gap;
                }
                else
                {
                    Entry.Pos.y = Cursor;
                    Cursor += Entry.Size.y + Gap;
                }
                Entry.Node->SetGridPos(Entry.Pos.x, Entry.Pos.y);
            }
            return;
        }

        for (FEntry& Entry : Entries)
        {
            switch (Alignment)
            {
                case ENodeAlignment::Left:    Entry.Pos.x = MinLeft;                              break;
                case ENodeAlignment::Right:   Entry.Pos.x = MaxRight - Entry.Size.x;              break;
                case ENodeAlignment::Top:     Entry.Pos.y = MinTop;                               break;
                case ENodeAlignment::Bottom:  Entry.Pos.y = MaxBottom - Entry.Size.y;             break;
                case ENodeAlignment::CenterX: Entry.Pos.x = CenterX - Entry.Size.x * 0.5f;        break;
                case ENodeAlignment::CenterY: Entry.Pos.y = CenterY - Entry.Size.y * 0.5f;        break;
                default: return;
            }
            Entry.Node->SetGridPos(Entry.Pos.x, Entry.Pos.y);
        }
    }

    void CEdNodeGraph::CollectContributingNodes(THashSet<CEdGraphNode*>& OutContributing) const
    {
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (Node != nullptr && IsGraphRootNode(Node))
            {
                GraphAlgorithms::CollectReachableFromRoot(Node, OutContributing);
            }
        }
    }

    void CEdNodeGraph::CollectDeadNodes(TVector<CEdGraphNode*>& OutDead) const
    {
        OutDead.clear();

        THashSet<CEdGraphNode*> Contributing;
        CollectContributingNodes(Contributing);

        // With no roots there is no way to tell live from dead, and reporting all of them would be worse.
        if (Contributing.empty())
        {
            return;
        }

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (Node != nullptr && !Node->IsCommentNode() && Contributing.find(Node) == Contributing.end())
            {
                OutDead.push_back(Node);
            }
        }
    }

    void CEdNodeGraph::SelectDeadNodes()
    {
        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);

        Editor.ClearSelection();
        for (CEdGraphNode* Node : Dead)
        {
            Editor.SelectNode(Node->GetNodeID(), true);
        }
    }

    void CEdNodeGraph::DeleteDeadNodes()
    {
        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);
        if (!Dead.empty())
        {
            SetPendingEditLabel("Delete Unused Nodes");
            RemoveNodes(Dead);
        }
    }

    void CEdNodeGraph::TidyGraph()
    {
        // Column spacing is generous since cramped columns make long wires ambiguous.
        constexpr float kColumnGap = 90.0f;
        constexpr float kRowGap    = 28.0f;

        THashSet<CEdGraphNode*> Contributing;
        CollectContributingNodes(Contributing);
        if (Contributing.empty())
        {
            return;
        }

        SetPendingEditLabel("Arrange Nodes");
        NotifyCosmeticChange();

        // Longest path, not shortest, so no wire skips backwards over a column.
        THashMap<CEdGraphNode*, int32> Depth;
        TVector<CEdGraphNode*> Sorted;
        GraphAlgorithms::TopologicalSortReachable(Nodes, Contributing, Sorted);

        // Walking Sorted backwards visits every consumer before its producers, as the relaxation needs.
        for (CEdGraphNode* Node : Sorted)
        {
            Depth[Node] = 0;
        }
        for (int32 i = (int32)Sorted.size() - 1; i >= 0; --i)
        {
            CEdGraphNode* Node = Sorted[i];
            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Connected : InputPin->GetConnections())
                {
                    CEdGraphNode* Producer = Connected->GetOwningNode();
                    if (Contributing.find(Producer) != Contributing.end())
                    {
                        Depth[Producer] = Math::Max(Depth[Producer], Depth[Node] + 1);
                    }
                }
            }
        }

        int32 MaxDepth = 0;
        for (const auto& Pair : Depth)
        {
            MaxDepth = Math::Max(MaxDepth, Pair.second);
        }

        // A single barycentric pass, stable and linear in edges, which removes the obvious tangles.
        TVector<TVector<CEdGraphNode*>> Columns;
        Columns.resize(MaxDepth + 1);
        for (const auto& Pair : Depth)
        {
            Columns[Pair.second].push_back(Pair.first);
        }

        const CEdGraphNode* Anchor = Sorted.empty() ? Nodes[0].Get() : Sorted.back();
        const ImVec2 Origin(Anchor->GetNodeX(), Anchor->GetNodeY());

        THashMap<CEdGraphNode*, float> PlacedY;

        // Roots are the rightmost column, so a column is ordered against consumers with final Y.
        float ColumnRight = Origin.x;
        for (int32 D = 0; D <= MaxDepth; ++D)
        {
            TVector<CEdGraphNode*>& Column = Columns[D];

            Algo::StableSort(Column, [&](CEdGraphNode* A, CEdGraphNode* B)
            {
                auto Barycenter = [&](CEdGraphNode* Node)
                {
                    float Sum = 0.0f;
                    int32 Count = 0;
                    for (const auto& OutputPinRef : Node->GetOutputPins())
                    {
                        CEdNodeGraphPin* OutputPin = OutputPinRef.Get();
                        for (CEdNodeGraphPin* Connected : OutputPin->GetConnections())
                        {
                            auto It = PlacedY.find(Connected->GetOwningNode());
                            if (It != PlacedY.end())
                            {
                                Sum += It->second;
                                ++Count;
                            }
                        }
                    }
                    return Count > 0 ? (Sum / (float)Count) : FLT_MAX;
                };
                return Barycenter(A) < Barycenter(B);
            });

            float WidestInColumn = 0.0f;
            for (CEdGraphNode* Node : Column)
            {
                WidestInColumn = Math::Max(WidestInColumn, Editor.GetNodeSize(Node->GetNodeID()).x);
            }

            const float ColumnX = ColumnRight - WidestInColumn;

            float CursorY = Origin.y;
            for (CEdGraphNode* Node : Column)
            {
                Node->SetGridPos(ColumnX, CursorY);
                PlacedY[Node] = CursorY;
                CursorY += Editor.GetNodeSize(Node->GetNodeID()).y + kRowGap;
            }

            ColumnRight = ColumnX - kColumnGap;
        }

        // Dead nodes park in their own column, so tidying never buries work-in-progress in the graph.
        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);
        if (!Dead.empty())
        {
            float WidestDead = 0.0f;
            for (CEdGraphNode* Node : Dead)
            {
                WidestDead = Math::Max(WidestDead, Editor.GetNodeSize(Node->GetNodeID()).x);
            }

            const float DeadX = ColumnRight - kColumnGap * 2.0f - WidestDead;
            float CursorY = Origin.y;
            for (CEdGraphNode* Node : Dead)
            {
                Node->SetGridPos(DeadX, CursorY);
                CursorY += Editor.GetNodeSize(Node->GetNodeID()).y + kRowGap;
            }
        }
    }

    void CEdNodeGraph::DrawAlignmentMenuItems()
    {
        struct FAlignEntry { ENodeAlignment Mode; const char* Label; const char* Shortcut; };

        static constexpr FAlignEntry Entries[] =
        {
            { ENodeAlignment::Left,        "Align Left",      "Shift+A" },
            { ENodeAlignment::Right,       "Align Right",     "Shift+D" },
            { ENodeAlignment::Top,         "Align Top",       "Shift+W" },
            { ENodeAlignment::Bottom,      "Align Bottom",    "Shift+S" },
            { ENodeAlignment::CenterX,     "Align Center X",  "Shift+X" },
            { ENodeAlignment::CenterY,     "Align Center Y",  "Shift+Y" },
            { ENodeAlignment::DistributeX, "Distribute X",    nullptr   },
            { ENodeAlignment::DistributeY, "Distribute Y",    nullptr   },
        };

        // Graying the whole set out beats offering items that silently do nothing.
        const bool bEnabled = Editor.GetSelectedNodeCount() >= 2;

        for (const FAlignEntry& Entry : Entries)
        {
            if (Entry.Mode == ENodeAlignment::DistributeX)
            {
                ImGui::Separator();
            }
            if (ImGui::MenuItem(Entry.Label, Entry.Shortcut, false, bEnabled))
            {
                AlignSelectedNodes(Entry.Mode);
            }
        }
    }

    void CEdNodeGraph::PlaceNodeAtScreen(CEdGraphNode* Node, ImVec2 ScreenPos)
    {
        if (Node != nullptr)
        {
            const ImVec2 CanvasPos = Editor.ScreenToCanvas(ScreenPos);
            Node->SetGridPos(CanvasPos.x, CanvasPos.y);
        }
    }

    void CEdNodeGraph::FocusNode(CEdGraphNode* Node)
    {
        if (Node != nullptr)
        {
            Editor.SelectNode(Node->GetNodeID(), false);
            Editor.NavigateToSelection(0.25f);
        }
    }

    void CEdNodeGraph::QueueFrameNodes(const TVector<int64>& NodeIDs)
    {
        PendingFrameNodes = NodeIDs;
        bHasPendingFrame  = true;
    }

    CEdNodeGraphPin* CEdNodeGraph::FindPinByGUID(uint32 PinID) const
    {
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (!Node.IsValid())
            {
                continue;
            }

            for (uint32 Direction = 0; Direction < (uint32)ENodePinDirection::Count; ++Direction)
            {
                if (CEdNodeGraphPin* Pin = Node->GetPin(PinID, (ENodePinDirection)Direction))
                {
                    return Pin;
                }
            }
        }

        return nullptr;
    }

    void CEdNodeGraph::DrawPinTooltip(CEdNodeGraphPin* Pin)
    {
        const FString TypeName = Pin->GetPinTypeName();
        if (!TypeName.empty())
        {
            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, 1.0f), "%s", TypeName.c_str());
            ImGui::SameLine();
        }

        ImGui::TextUnformatted(Pin->GetPinName().c_str());
    }

    uint64 CEdNodeGraph::MakeLinkID(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin)
    {
        return ((uint64)OutputPin->GetPinGUID() << 32) | (uint64)InputPin->GetPinGUID();
    }

    void CEdNodeGraph::BeginCanvasNode(CEdGraphNode* Node, const FGraphNodeStyle& Style)
    {
        ImVec2 Position(Node->GetNodeX(), Node->GetNodeY());
        Editor.BeginNode(Node->GetNodeID(), Position, Style);
        SyncNodePosition(Node, Position);
    }

    void CEdNodeGraph::SyncNodePosition(CEdGraphNode* Node, ImVec2 Position)
    {
        if (Position.x != Node->GetNodeX() || Position.y != Node->GetNodeY())
        {
            Node->SetGridPos(Position.x, Position.y);
            bLayoutChangedSinceImage = true;
        }
    }

    ImU32 CEdNodeGraph::GetWireColor(const CEdNodeGraphPin* OutputPin) const
    {
        // Followed back through reroutes to the output that makes the value, with a hop limit in case reroutes loop.
        for (int32 Hop = 0; OutputPin != nullptr && Hop < 64; ++Hop)
        {
            const CEdGraphNode* Owner = OutputPin->GetOwningNode();
            const CEdNodeGraphPin* Source = Owner != nullptr && Owner->IsRerouteNode() ? Owner->GetRerouteSourcePin() : nullptr;
            if (Source == nullptr || !Source->HasConnection())
            {
                break;
            }
            OutputPin = Source->Connections[0];
        }
        return OutputPin != nullptr ? OutputPin->GetPinColor() : IM_COL32_WHITE;
    }

    void CEdNodeGraph::GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const
    {
        OutColor     = GetWireColor(OutputPin);
        OutThickness = GDefaultWireThickness;
    }

    void CEdNodeGraph::DrawStandardNode(CEdGraphNode* Node)
    {
        FGraphNodeBuilder NodeBuilder(Editor);

        ImVec2 Position(Node->GetNodeX(), Node->GetNodeY());
        NodeBuilder.Begin(Node->GetNodeID(), Position);
        SyncNodePosition(Node, Position);

        if (!Node->WantsTitlebar())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        }

        const bool bNodeActive = DebugContext.bEnabled && DebugContext.ActiveNodes != nullptr
            && DebugContext.ActiveNodes->find(Node) != DebugContext.ActiveNodes->end();
        uint32 HeaderColor = bNodeActive ? IM_COL32(235, 170, 40, 255) : Node->GetNodeTitleColor();
        if (Node->HasError())
        {
            HeaderColor = GNodeErrorHeaderColor;
        }
        else if (Node->HasWarning())
        {
            HeaderColor = GNodeWarningHeaderColor;
        }
        NodeBuilder.Header(HeaderColor);

        if (!Node->WantsTitlebar())
        {
            ImGui::PopStyleVar();
        }

        ImGui::Spring(0);
        Node->DrawNodeTitleBar();

        if (bDebug)
        {
            ImGui::Text("(ID - %lld)", (long long)Node->GetNodeID());
        }

        ImGui::Spring(1);
        ImGui::Dummy(ImVec2(Node->GetMinNodeTitleBarSize()));
        ImGui::Spring(0);
        NodeBuilder.EndHeader();

        if (Node->GetInputPins().empty())
        {
            ImGui::BeginVertical("inputs", ImVec2(0, 0), 0.0f);
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::EndVertical();
        }

        const ImVec4 DisabledText(0.5f, 0.5f, 0.5f, 0.6f);

        for (const auto& InputPinRef : Node->GetInputPins())
        {
            CEdNodeGraphPin* InputPin = InputPinRef.Get();
            ImU32 PinColor = InputPin->HasConnection() ? GetWireColor(InputPin->GetConnection(0)) : InputPin->GetPinColor();
            PinColor = Node->HasError() ? GNodeErrorPinColor : PinColor;
            const bool  bDisabled = InputPin->IsDisabled();

            NodeBuilder.Input(InputPin->GetPinGUID(), PinColor);
            ImGui::PushID(InputPin);
            if (bDisabled)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, DisabledText);
            }

            FGraphNodeBuilder::PinIcon(MakePinIcon(InputPin, PinColor, bDisabled));
            ImGui::Spring(0);
            ImGui::TextUnformatted(InputPin->GetPinName().c_str());

            // Inline editors for unconnected pins, opt-in per graph through ShouldDrawInlinePinEditors().
            if (ShouldDrawInlinePinEditors() && !InputPin->HasConnection() && InputPin->HasInlineEditor())
            {
                ImGui::Spring(1.0f, 12.0f);
                InputPin->DrawPin();
            }

            DrawPinDebugValue(InputPin);
            if (bDebug)
            {
                ImGui::Text("(ID - %i)", InputPin->GetPinGUID());
            }
            ImGui::Spring(0);

            if (bDisabled)
            {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
            NodeBuilder.EndInput();
        }

        NodeBuilder.Middle();
        Node->DrawNodeBody();

        for (const auto& OutputPinRef : Node->GetOutputPins())
        {
            CEdNodeGraphPin* OutputPin = OutputPinRef.Get();
            const ImU32 PinColor  = Node->HasError() ? GNodeErrorPinColor : OutputPin->GetPinColor();
            const bool  bDisabled = OutputPin->IsDisabled();

            NodeBuilder.Output(OutputPin->GetPinGUID(), PinColor);
            ImGui::PushID(OutputPin);
            ImGui::Spring(0);
            if (bDisabled)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, DisabledText);
            }

            ImGui::Spring(1, 1);
            DrawPinDebugValue(OutputPin);
            ImGui::TextUnformatted(OutputPin->GetPinName().c_str());
            ImGui::Spring(0);
            FGraphNodeBuilder::PinIcon(MakePinIcon(OutputPin, PinColor, bDisabled));

            if (bDebug)
            {
                ImGui::Text("(ID - %i)", OutputPin->GetPinGUID());
            }
            if (bDisabled)
            {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
            NodeBuilder.EndOutput();
        }

        DrawNodePreview(Node, NodeBuilder);
        NodeBuilder.End(Node->WantsTitlebar());

        float   Heat = 0.0f;
        FString HeatLabel;
        if (GetNodeHeat(Node, Heat, HeatLabel))
        {
            DrawNodeHeat(Node, Heat, HeatLabel);
        }

        if (IsNodePreviewed(Node))
        {
            constexpr float Radius = 11.0f;
            const ImVec2 Corner(Node->GetNodeX() + Editor.GetNodeSize(Node->GetNodeID()).x, Node->GetNodeY());
            ImDrawList* DrawList = Editor.GetNodeBackgroundDrawList(Node->GetNodeID());
            DrawList->AddCircleFilled(Corner, Radius, EditorColors::U32(EditorColors::Accent()));
            DrawList->AddText(ImGuiX::CenteredIconPos(LE_ICON_EYE, Corner - ImVec2(Radius, Radius), Corner + ImVec2(Radius, Radius)), IM_COL32_WHITE, LE_ICON_EYE);
        }

        if (Node->HasError())
        {
            DrawNodeErrorBubble(Node);
        }
    }

    void CEdNodeGraph::DrawNodePreview(CEdGraphNode* Node, FGraphNodeBuilder& Builder)
    {
        if (!HasNodePreview(Node))
        {
            return;
        }

        Builder.Footer();
        if (Node->bShowPreview)
        {
            // A full-width row of its own, since the node's vertical layout re-aligns a narrower item by moving vertices it drew.
            ImGui::BeginHorizontal("Preview");
            ImGui::Spring(1.0f, 0.0f);
            DrawNodePreviewContent(Node);
            ImGui::Spring(1.0f, 0.0f);
            ImGui::EndHorizontal();
        }

        if (Builder.ExpandToggle(Node->bShowPreview))
        {
            SetPendingEditLabel(Node->bShowPreview ? FName("Hide Node Preview") : FName("Show Node Preview"));
            Node->bShowPreview = !Node->bShowPreview;
            NotifyCosmeticChange();
        }
    }

    void CEdNodeGraph::CollectLinks(TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& OutLinks) const
    {
        OutLinks.clear();
        OutLinks.reserve(Nodes.size() * 2);
        for (const auto& NodeRef : Nodes)
        {
            for (const auto& InputPinRef : NodeRef->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    OutLinks.emplace_back(InputPin, Connection);
                }
            }
        }
    }

    void CEdNodeGraph::DrawGraph()
    {
        LUMINA_PROFILE_SCOPE();

        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Links;
        CollectLinks(Links);

        // The view is not saved with the asset, so the content is framed once every node has been measured.
        if (bNeedsInitialFraming && Editor.HasDrawn())
        {
            bNeedsInitialFraming = false;
            Editor.NavigateToContent(0.0f);
        }

        if (bHasPendingFrame)
        {
            bHasPendingFrame = false;
            if (!PendingFrameNodes.empty())
            {
                Editor.ClearSelection();
            }

            ImRect Bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
            for (const auto& NodeRef : Nodes)
            {
                const int64 FrameID = NodeRef->GetNodeID();
                if (!PendingFrameNodes.empty())
                {
                    if (Algo::Find(PendingFrameNodes, FrameID) == PendingFrameNodes.end())
                    {
                        continue;
                    }
                    Editor.SelectNode(FrameID, true);
                }
                Bounds.Add(GetNodeBounds(NodeRef.Get()));
            }

            // Instant rather than animated, so a caller reading the view back next frame sees where it landed.
            if (Bounds.Min.x <= Bounds.Max.x)
            {
                Bounds.Expand(60.0f);
                Editor.NavigateTo(Bounds, 0.0f);
            }
            PendingFrameNodes.clear();
        }

        Editor.Begin(GetName().c_str(), (int32)Nodes.size());

        const FGraphLinkDrag* PinDrag = Editor.GetLinkDrag();
        DragSourcePin   = PinDrag != nullptr ? FindPinByGUID(PinDrag->FromPin) : nullptr;
        bDragMovesLinks = PinDrag != nullptr && PinDrag->bMoveLinks;

        // Skipped when the graph declares no root, which keeps it off graphs with no dead-node notion.
        THashSet<CEdGraphNode*> ContributingNodes;
        if (bFadeDeadNodes)
        {
            CollectContributingNodes(ContributingNodes);
        }

        DrawGraphOverlay(Links);

        // Larger comments go first, so one nested inside another draws over it.
        TVector<CEdNode_Comment*> Comments;
        for (const auto& NodeRef : Nodes)
        {
            if (NodeRef->IsCommentNode())
            {
                Comments.push_back(static_cast<CEdNode_Comment*>(NodeRef.Get()));
            }
        }
        Algo::StableSort(Comments, [](const CEdNode_Comment* A, const CEdNode_Comment* B) { return A->Width * A->Height > B->Width * B->Height; });
        for (CEdNode_Comment* Comment : Comments)
        {
            DrawCommentNode(Comment);
        }

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (Node->IsCommentNode())
            {
                continue;
            }

            // This node reaches no output, so the compiler never emits it, and it costs nothing when connected.
            const bool bDeadNode = (!ContributingNodes.empty() && !ContributingNodes.contains(Node)) || IsNodeIdle(Node);
            if (bDeadNode)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
            }

            if (Node->WantsRerouteDotRendering())
            {
                DrawRerouteNode(Node);
            }
            else if (!DrawCustomNode(Node))
            {
                DrawStandardNode(Node);
            }

            if (bDeadNode)
            {
                ImGui::PopStyleVar();
            }
        }

        const bool bFlowLinks = DebugContext.bEnabled && DebugContext.bFlowLinks;
        for (const auto& [InputPin, OutputPin] : Links)
        {
            ImU32 Color     = IM_COL32_WHITE;
            float Thickness = GDefaultWireThickness;
            GetLinkStyle(InputPin, OutputPin, Color, Thickness);
            Editor.Link(MakeLinkID(InputPin, OutputPin), OutputPin->GetPinGUID(), InputPin->GetPinGUID(), Color, Thickness,
                bFlowLinks || WantsLinkFlow(InputPin, OutputPin));

            FString Badge;
            ImU32   BadgeColor = 0;
            if (GetLinkBadge(InputPin, OutputPin, Badge, BadgeColor))
            {
                Editor.LinkBadge(Badge, BadgeColor);
            }
        }
        SubmitZipTies();

        Editor.End();

        DrawCommentHints();

        // A request that rewired or deleted leaves the collected pairs naming pins that may be gone.
        const uint64 VersionBeforeRequests = ContentVersion;
        HandleCanvasRequests(Links);
        if (ContentVersion != VersionBeforeRequests)
        {
            CollectLinks(Links);
        }
        DrawCanvasPopups(Links);

        // Reporting none lets consumers fall back rather than picking an arbitrary selection member.
        if (NodeSelectedCallback)
        {
            NodeSelectedCallback(Editor.GetSelectedNodeCount() == 1 ? FindNode(Editor.GetSelectedNodes()[0]) : nullptr);
        }

        if (LinkSelectedCallback)
        {
            const TVector<uint64> SelectedLinks = Editor.GetSelectedLinks();
            const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Selected = SelectedLinks.empty() ? nullptr : FindLink(Links, SelectedLinks[0]);
            LinkSelectedCallback(Selected != nullptr ? Selected->first : nullptr, Selected != nullptr ? Selected->second : nullptr);
        }

        Editor.EndWindow();

        // The host window is current again, and a spawned node is placed under the cursor.
        DrawCanvasDropTarget();

        // The first draw measures every node, which is opening the asset, not an edit.
        if (bFirstDraw)
        {
            RebaseUndoImage();
        }
        else
        {
            CommitFrameEdits();
        }

        bFirstDraw = false;
    }

    void CEdNodeGraph::HandleCanvasRequests(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        if (const uint32 HoveredPinID = Editor.GetHoveredPin())
        {
            if (CEdNodeGraphPin* HoveredPin = FindPinByGUID(HoveredPinID))
            {
                ImGui::BeginTooltip();
                DrawPinTooltip(HoveredPin);
                ImGui::EndTooltip();
            }
        }
        else if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* HoveredLink = FindLink(Links, Editor.GetHoveredLink()))
        {
            DrawLinkTooltip(HoveredLink->first, HoveredLink->second);
        }

        if (!Editor.GetTooltip().empty())
        {
            ImGui::SetTooltip("%s", Editor.GetTooltip().c_str());
        }

        const FGraphContextRequest& Context = Editor.GetContextRequest();
        switch (Context.Item)
        {
            case EGraphItem::Node:
            {
                ContextMenuNodeID = (int64)Context.ID;
                ImGui::OpenPopup("Node Context Menu");
                break;
            }
            case EGraphItem::Pin:
            {
                ContextMenuPinID = (uint32)Context.ID;
                ImGui::OpenPopup("Pin Context Menu");
                break;
            }
            case EGraphItem::Link:
            {
                ContextMenuLinkID = Context.ID;
                ImGui::OpenPopup("Link Context Menu");
                break;
            }
            case EGraphItem::ZipTie:
            {
                ContextMenuTieID = (uint32)Context.ID;
                ImGui::OpenPopup("Zip Tie Context Menu");
                break;
            }
            case EGraphItem::Background:
            {
                PendingSourcePin = nullptr;
                ActionMenu.Reset();
                ImGui::OpenPopup("Create New Node");
                break;
            }
            default:
                break;
        }

        // Double-clicking a comment edits its title, and any other node descends into its sub-graph if it has one.
        if (CEdGraphNode* DoubleClicked = FindNode(Editor.GetDoubleClickedNode()))
        {
            if (DoubleClicked->IsCommentNode())
            {
                BeginRenameNode(DoubleClicked);
            }
            else if (NodeDoubleClickedCallback)
            {
                NodeDoubleClickedCallback(DoubleClicked);
            }
        }

        if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* DoubleClickedLink = FindLink(Links, Editor.GetDoubleClickedLink()))
        {
            if (GetRerouteNodeClass() != nullptr)
            {
                SetPendingEditLabel("Insert Reroute");
                InsertRerouteOnLink(DoubleClickedLink->second, DoubleClickedLink->first, Editor.ScreenToCanvas(ImGui::GetMousePos()));
            }
        }

        if (CEdNodeGraphPin* Pin = Editor.GetBreakLinksPin() != 0 ? FindPinByGUID(Editor.GetBreakLinksPin()) : nullptr)
        {
            SetPendingEditLabel("Break Links");
            BreakPinLinks(Pin);
        }

        HandleLinkDrag();
        HandleStroke(Links);
        HandleSplice(Links);

        // A letter or digit held while clicking empty canvas drops that node there; the key is the modifier, the click the trigger.
        ImVec2 ClickPos;
        if (Editor.GetBackgroundClick(ClickPos))
        {
            for (int32 Key = ImGuiKey_A; Key <= ImGuiKey_Z; ++Key)
            {
                if (ImGui::IsKeyDown((ImGuiKey)Key))
                {
                    HandleQuickPlace((char)('A' + (Key - ImGuiKey_A)), ClickPos);
                    break;
                }
            }

            for (int32 Digit = 0; Digit <= 9; ++Digit)
            {
                if (ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_0 + Digit)))
                {
                    HandleQuickPlace(Digit, ClickPos);
                    break;
                }
            }
        }

        if (Editor.WasShortcutPressed(EGraphShortcut::Copy) || Editor.WasShortcutPressed(EGraphShortcut::Cut))
        {
            CollectSelectedNodesForClone(GetNodeClipboard(), GClipboardPivot);
        }

        if (Editor.WasShortcutPressed(EGraphShortcut::Cut) || Editor.WasShortcutPressed(EGraphShortcut::Delete))
        {
            DeleteSelection(Links);
        }

        if (Editor.WasShortcutPressed(EGraphShortcut::Paste))
        {
            PasteClipboard(Editor.ScreenToCanvas(ImGui::GetMousePos()));
        }

        if (Editor.WasShortcutPressed(EGraphShortcut::Duplicate))
        {
            // A local pivot, since duplicate never leaves this graph and must not disturb the shared one.
            TVector<CEdGraphNode*> DuplicateNodes;
            ImVec2 DuplicatePivot;
            CollectSelectedNodesForClone(DuplicateNodes, DuplicatePivot);

            Editor.ClearSelection();
            SetPendingEditLabel("Duplicate Nodes");
            CloneNodes(DuplicateNodes, Editor.ScreenToCanvas(ImGui::GetMousePos()) - DuplicatePivot);
        }

        // Shift and a letter, since keyboard nav is enabled app-wide and ImGui eats the arrows.
        if (!ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt)
        {
            struct FAlignHotkey { ImGuiKey Key; ENodeAlignment Mode; };
            static constexpr FAlignHotkey Hotkeys[] =
            {
                { ImGuiKey_A, ENodeAlignment::Left    },
                { ImGuiKey_D, ENodeAlignment::Right   },
                { ImGuiKey_W, ENodeAlignment::Top     },
                { ImGuiKey_S, ENodeAlignment::Bottom  },
                { ImGuiKey_X, ENodeAlignment::CenterX },
                { ImGuiKey_Y, ENodeAlignment::CenterY },
            };

            for (const FAlignHotkey& Hotkey : Hotkeys)
            {
                if (ImGui::IsKeyPressed(Hotkey.Key, false))
                {
                    AlignSelectedNodes(Hotkey.Mode);
                    break;
                }
            }
        }

        HandleRenameShortcut();
        HandleCommentShortcut();
        HandleGraphHotkeys();
    }

    EGraphPinHighlight CEdNodeGraph::GetPinDragHighlight(CEdNodeGraphPin* Pin) const
    {
        if (DragSourcePin == nullptr || Pin == DragSourcePin)
        {
            return EGraphPinHighlight::None;
        }

        // Moved wires land on a pin facing the same way as the one they leave.
        if (bDragMovesLinks)
        {
            return Pin->bInputPin == DragSourcePin->bInputPin && !Pin->IsDisabled() ? EGraphPinHighlight::Accepts : EGraphPinHighlight::Refuses;
        }

        if (Pin->bInputPin == DragSourcePin->bInputPin || Pin->IsDisabled() || Pin->GetOwningNode() == DragSourcePin->GetOwningNode())
        {
            return EGraphPinHighlight::Refuses;
        }

        CEdNodeGraphPin* Output = Pin->bInputPin ? DragSourcePin : Pin;
        CEdNodeGraphPin* Input  = Pin->bInputPin ? Pin : DragSourcePin;
        return GetSchema().CanCreateConnection(Output, Input) ? EGraphPinHighlight::Accepts : EGraphPinHighlight::Refuses;
    }

    FGraphPinIcon CEdNodeGraph::MakePinIcon(CEdNodeGraphPin* Pin, ImU32 Color, bool bDisabled) const
    {
        const ImU32 Alpha = bDisabled ? 80 : 255;

        FGraphPinIcon Icon;
        Icon.Color      = (Color & ~IM_COL32_A_MASK) | (Alpha << IM_COL32_A_SHIFT);
        Icon.InnerColor = IM_COL32(32, 32, 32, Alpha);
        Icon.Shape      = Pin->GetPinShape();
        Icon.Highlight  = GetPinDragHighlight(Pin);
        Icon.bConnected = Pin->HasConnection();
        return Icon;
    }

    bool CEdNodeGraph::FindSplicePins(const FGraphSplice& Splice, const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links,
                                      CEdNodeGraphPin*& OutInput, CEdNodeGraphPin*& OutOutput) const
    {
        const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, Splice.LinkID);
        CEdGraphNode* Node = FindNode(Splice.NodeID);
        if (Link == nullptr || Node == nullptr)
        {
            return false;
        }

        OutInput  = FindAutoConnectPin(Node, Link->second);
        OutOutput = FindAutoConnectPin(Node, Link->first);
        return OutInput != nullptr && OutOutput != nullptr && !OutInput->HasConnection();
    }

    void CEdNodeGraph::HandleSplice(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        CEdNodeGraphPin* NodeInput  = nullptr;
        CEdNodeGraphPin* NodeOutput = nullptr;
        if (const FGraphSplice* Candidate = Editor.GetSpliceCandidate())
        {
            Editor.AcceptSplice(FindSplicePins(*Candidate, Links, NodeInput, NodeOutput));
        }

        const FGraphSplice* Drop = Editor.GetSpliceDrop();
        if (Drop == nullptr || !FindSplicePins(*Drop, Links, NodeInput, NodeOutput))
        {
            return;
        }

        const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, Drop->LinkID);
        CEdNodeGraphPin* WireInput  = Link->first;
        CEdNodeGraphPin* WireOutput = Link->second;

        SetPendingEditLabel("Insert Node");
        BreakLink(WireInput, WireOutput);
        ConnectPins(WireOutput, NodeInput);
        ConnectPins(NodeOutput, WireInput);
    }

    void CEdNodeGraph::HandleGraphHotkeys()
    {
        const ImGuiIO& IO = ImGui::GetIO();
        if (ImGui::IsAnyItemActive() || IO.WantTextInput || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            return;
        }

        if (IO.KeyCtrl && !IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            bOpenFind = true;
        }

        if (!IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Q, false))
        {
            StraightenSelection();
        }

        for (int32 Slot = 1; Slot <= 9 && (IO.KeyCtrl || IO.KeyShift); ++Slot)
        {
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + Slot), false))
            {
                if (IO.KeyCtrl)
                {
                    SetBookmark(Slot);
                }
                else
                {
                    RecallBookmark(Slot);
                }
            }
        }
    }

    void CEdNodeGraph::StraightenSelection()
    {
        TVector<int64> Targets = Editor.GetSelectedNodes();
        if (Targets.empty() && Editor.GetHoveredNode() != 0)
        {
            Targets.push_back(Editor.GetHoveredNode());
        }

        bool bMoved = false;
        for (int64 NodeID : Targets)
        {
            CEdGraphNode* Node = FindNode(NodeID);
            if (Node == nullptr || Node->IsCommentNode())
            {
                continue;
            }

            // The first wire in is the one a reader follows, and a node fed by nothing lines up with what it feeds.
            CEdNodeGraphPin* Pin = nullptr;
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Input : Node->GetInputPins())
            {
                if (Input->HasConnection())
                {
                    Pin = Input.Get();
                    break;
                }
            }
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Output : Node->GetOutputPins())
            {
                if (Pin == nullptr && Output->HasConnection())
                {
                    Pin = Output.Get();
                }
            }

            ImVec2 PinAnchor;
            ImVec2 FarAnchor;
            if (Pin == nullptr || !Editor.GetPinAnchor(Pin->GetPinGUID(), PinAnchor) || !Editor.GetPinAnchor(Pin->GetConnection(0)->GetPinGUID(), FarAnchor))
            {
                continue;
            }

            if (!bMoved)
            {
                SetPendingEditLabel("Straighten Wires");
                NotifyCosmeticChange();
                bMoved = true;
            }
            Node->SetGridPos(Node->GetNodeX(), Node->GetNodeY() + FarAnchor.y - PinAnchor.y);
        }
    }

    void CEdNodeGraph::SetBookmark(int32 Slot)
    {
        ImVec2 Center;
        float  Zoom = 1.0f;
        Editor.GetView(Center, Zoom);

        auto Existing = Algo::FindIf(Bookmarks, [Slot](const SGraphBookmark& Bookmark) { return Bookmark.Slot == Slot; });
        SGraphBookmark& Bookmark = Existing != Bookmarks.end() ? *Existing : Bookmarks.emplace_back();
        Bookmark.Slot = Slot;
        Bookmark.X    = Center.x;
        Bookmark.Y    = Center.y;
        Bookmark.Zoom = Zoom;

        SetPendingEditLabel("Set Bookmark");
        NotifyCosmeticChange();
        ImGuiX::Notifications::NotifyInfo("Bookmark {0} set. Shift+{0} comes back here.", Slot);
    }

    void CEdNodeGraph::RecallBookmark(int32 Slot)
    {
        auto Existing = Algo::FindIf(Bookmarks, [Slot](const SGraphBookmark& Bookmark) { return Bookmark.Slot == Slot; });
        if (Existing == Bookmarks.end())
        {
            ImGuiX::Notifications::NotifyInfo("Nothing on bookmark {0} yet. Ctrl+{0} saves the current view there.", Slot);
            return;
        }
        Editor.NavigateToView(ImVec2(Existing->X, Existing->Y), Existing->Zoom, 0.25f);
    }

    void CEdNodeGraph::DrawFindPopup()
    {
        constexpr int32 MaxResults           = 12;
        constexpr int32 DisplayNamePenalty   = 50;

        if (bOpenFind)
        {
            bOpenFind     = false;
            FindQuery[0]  = '\0';
            FindSelection = 0;
            const ImVec2 WindowPos  = ImGui::GetWindowPos();
            const ImVec2 WindowSize = ImGui::GetWindowSize();
            ImGui::SetNextWindowPos(ImVec2(WindowPos.x + WindowSize.x * 0.5f, WindowPos.y + ImGui::GetFontSize() * 2.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::OpenPopup("Find Node");
        }

        if (!ImGui::BeginPopup("Find Node"))
        {
            return;
        }

        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        const bool bSubmitted = ImGui::InputTextWithHint("##Find", LE_ICON_MAGNIFY " Find a node by title or type", FindQuery, sizeof(FindQuery), ImGuiInputTextFlags_EnterReturnsTrue);

        struct FMatch
        {
            CEdGraphNode* Node;
            FString       Title;
            int32         Score;
        };

        TVector<FMatch>  Matches;
        TVector<uint16>  Indices;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (Node == nullptr || Node->IsRerouteNode())
            {
                continue;
            }

            int32 Score = 0;
            FString Title = Node->GetNodeTitleText();
            if (FindQuery[0] == '\0' || FuzzyMatch(FindQuery, Title.c_str(), Score, Indices))
            {
                Matches.push_back(FMatch{ Node, Move(Title), Score });
            }
            else if (FuzzyMatch(FindQuery, FString(Node->GetNodeDisplayName()).c_str(), Score, Indices))
            {
                Matches.push_back(FMatch{ Node, Move(Title), Score - DisplayNamePenalty });
            }
        }
        Algo::StableSort(Matches, [](const FMatch& A, const FMatch& B) { return A.Score > B.Score; });

        const int32 Shown = Math::Min((int32)Matches.size(), MaxResults);
        FindSelection += ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ? 1 : 0;
        FindSelection -= ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? 1 : 0;
        FindSelection  = Shown > 0 ? Math::Clamp(FindSelection, 0, Shown - 1) : 0;

        CEdGraphNode* Chosen = bSubmitted && Shown > 0 ? Matches[FindSelection].Node : nullptr;

        // The arrows move this highlight, so keyboard nav must not pull focus off the search field onto a row.
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int32 Index = 0; Index < Shown; ++Index)
        {
            const FMatch& Match = Matches[Index];
            ImGui::PushID(Match.Node);
            if (ImGui::Selectable(Match.Title.c_str(), Index == FindSelection))
            {
                Chosen = Match.Node;
            }
            const FStringView TypeName = Match.Node->GetNodeDisplayName();
            if (Match.Title != TypeName)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.*s", (int)TypeName.size(), TypeName.data());
            }
            ImGui::PopID();
        }
        ImGui::PopItemFlag();
        if (Shown == 0)
        {
            ImGui::TextDisabled("No node matches.");
        }

        if (Chosen != nullptr)
        {
            FocusNode(Chosen);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void CEdNodeGraph::DrawGeneratedCodePopup()
    {
        if (bOpenCodeView)
        {
            bOpenCodeView = false;
            ImGui::OpenPopup("Generated Code");
        }

        CEdGraphNode* Node = CodeViewNodeID != 0 ? FindNode(CodeViewNodeID) : nullptr;
        FString Code;
        if (Node == nullptr || !GetNodeGeneratedCode(Node, Code))
        {
            CodeViewNodeID = 0;
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("Generated Code"))
        {
            CodeViewNodeID = 0;
            return;
        }

        ImGui::TextDisabled("%s", Node->GetNodeTitleText().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(LE_ICON_CONTENT_COPY " Copy"))
        {
            ImGui::SetClipboardText(Code.c_str());
        }
        ImGui::Separator();

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Mono);
        const float Lines  = (float)(Algo::Count(Code, '\n') + 1);
        const float Height = Math::Min(ImGui::GetTextLineHeightWithSpacing() * Lines, ImGui::GetFontSize() * 24.0f);
        ImGui::BeginChild("##Code", ImVec2(0.0f, Height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(Code.c_str());
        ImGui::EndChild();
        ImGuiX::Font::PopFont();
        ImGui::EndPopup();
    }

    void CEdNodeGraph::DrawNodeHeat(CEdGraphNode* Node, float Heat, const FString& Label)
    {
        constexpr float BarHeight = 3.0f;
        constexpr float Inset     = 6.0f;

        // Green for cheap, amber in the middle, red for the node costing the most.
        const ImVec4 Cool(0.35f, 0.80f, 0.47f, 1.0f);
        const ImVec4 Warm(0.94f, 0.75f, 0.27f, 1.0f);
        const ImVec4 Hot(0.92f, 0.33f, 0.27f, 1.0f);
        const float  Clamped = Math::Saturate(Heat);
        const ImU32  Color   = EditorColors::U32(Clamped < 0.5f ? ImLerp(Cool, Warm, Clamped * 2.0f) : ImLerp(Warm, Hot, (Clamped - 0.5f) * 2.0f));

        const ImVec2 Min(Node->GetNodeX(), Node->GetNodeY());
        const ImVec2 Size = Editor.GetNodeSize(Node->GetNodeID());
        const float  Width = Math::Max((Size.x - Inset * 2.0f) * Clamped, BarHeight);

        ImDrawList* DrawList = Editor.GetNodeBackgroundDrawList(Node->GetNodeID());
        DrawList->AddRectFilled(ImVec2(Min.x + Inset, Min.y + Size.y - BarHeight - 2.0f), ImVec2(Min.x + Inset + Width, Min.y + Size.y - 2.0f), Color, BarHeight * 0.5f);

        const ImVec2 TextSize = ImGui::CalcTextSize(Label.c_str());
        DrawList->AddText(ImVec2(Min.x + Size.x - TextSize.x - Inset, Min.y + Inset * 0.5f), Color, Label.c_str());
    }

    void CEdNodeGraph::HandleLinkDrag()
    {
        const FGraphLinkDrag* Drag = Editor.GetLinkDrag();
        if (Drag == nullptr)
        {
            return;
        }

        CEdNodeGraphPin* FromPin = FindPinByGUID(Drag->FromPin);
        if (FromPin == nullptr || FromPin->IsDisabled())
        {
            Editor.RejectLinkDrag();
            return;
        }

        if (Drag->bMoveLinks)
        {
            CEdNodeGraphPin* Target = Drag->ToPin != 0 ? FindPinByGUID(Drag->ToPin) : nullptr;
            const TVector<CEdNodeGraphPin*> Moving = FromPin->GetConnections();
            const FEdGraphSchema& Schema = GetSchema();
            const bool bValid = Target != nullptr && Target->bInputPin == FromPin->bInputPin && !Target->IsDisabled()
                && Algo::AllOf(Moving, [&](CEdNodeGraphPin* Other)
                {
                    return Target->bInputPin ? Schema.CanCreateConnection(Other, Target) : Schema.CanCreateConnection(Target, Other);
                });

            if (Drag->ToPin != 0 && !bValid)
            {
                Editor.RejectLinkDrag();
            }

            if (Drag->bDropped && bValid)
            {
                SetPendingEditLabel("Move Wires");
                for (CEdNodeGraphPin* Other : Moving)
                {
                    BreakLink(FromPin, Other);
                    if (Target->bInputPin)
                    {
                        ConnectPins(Other, Target);
                    }
                    else
                    {
                        ConnectPins(Target, Other);
                    }
                }
            }
            return;
        }

        // Dropped on a node rather than a pin, the wire goes to the first pin there that takes it.
        CEdNodeGraphPin* ToPin = Drag->ToPin != 0 ? FindPinByGUID(Drag->ToPin) : nullptr;
        if (ToPin == nullptr && Drag->ToNode != 0)
        {
            CEdGraphNode* TargetNode = FindNode(Drag->ToNode);
            if (TargetNode != nullptr && TargetNode != FromPin->GetOwningNode())
            {
                ToPin = FindAutoConnectPin(TargetNode, FromPin);
            }
        }

        CEdNodeGraphPin* Output = FromPin->bInputPin ? ToPin : FromPin;
        CEdNodeGraphPin* Input  = FromPin->bInputPin ? FromPin : ToPin;
        const bool bOverTarget = Drag->ToPin != 0 || Drag->ToNode != 0;
        const bool bValid = Output != nullptr && Input != nullptr && !Output->bInputPin && Input->bInputPin
            && !Output->IsDisabled() && !Input->IsDisabled() && GetSchema().CanCreateConnection(Output, Input);

        if (bOverTarget && !bValid)
        {
            Editor.RejectLinkDrag();
        }

        if (!Drag->bDropped)
        {
            return;
        }

        if (bValid)
        {
            SetPendingEditLabel("Connect Pins");
            ConnectPins(Output, Input);
        }
        else if (!bOverTarget)
        {
            PendingSourcePin = FromPin;
            ActionMenu.Reset();
            ImGui::OpenPopup("Create New Node");
        }
    }

    void CEdNodeGraph::HandleStroke(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        TVector<FGraphStrokeHit> Hits;
        const EGraphStroke Stroke = Editor.GetFinishedStroke(Hits);
        if (Stroke == EGraphStroke::None)
        {
            return;
        }

        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Crossed;
        ImVec2 Center(0.0f, 0.0f);
        for (const FGraphStrokeHit& Hit : Hits)
        {
            if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, Hit.LinkID))
            {
                Crossed.push_back(*Link);
                Center += Hit.Point;
            }
        }
        if (Crossed.empty())
        {
            return;
        }
        Center /= (float)Crossed.size();

        switch (Stroke)
        {
            case EGraphStroke::Cut:
            {
                SetPendingEditLabel(Crossed.size() == 1 ? FName("Cut Wire") : FName("Cut Wires"));
                for (const auto& [InputPin, OutputPin] : Crossed)
                {
                    BreakLink(InputPin, OutputPin);
                }
                break;
            }
            case EGraphStroke::Reroute:
            {
                if (GetRerouteNodeClass() == nullptr)
                {
                    ImGuiX::Notifications::NotifyWarning("This graph has no reroute nodes.");
                    break;
                }

                SetPendingEditLabel("Insert Reroutes");
                for (const FGraphStrokeHit& Hit : Hits)
                {
                    if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, Hit.LinkID))
                    {
                        InsertRerouteOnLink(Link->second, Link->first, Hit.Point);
                    }
                }
                break;
            }
            case EGraphStroke::ZipTie:
            {
                SetPendingEditLabel("Zip Tie Wires");
                AddZipTie(Crossed, Center);
                break;
            }
            default:
                break;
        }
    }

    void CEdNodeGraph::DeleteSelection(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        // Wires go first, while every pin they name is still alive.
        for (uint64 LinkID : Editor.GetSelectedLinks())
        {
            if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, LinkID))
            {
                SetPendingEditLabel("Break Link");
                BreakLink(Link->first, Link->second);
            }
        }

        const TVector<uint64> Ties = Editor.GetSelectedZipTies();
        if (!Ties.empty())
        {
            SetPendingEditLabel("Remove Zip Tie");
            ZipTies.erase(Algo::RemoveIf(ZipTies, [&Ties](const SGraphZipTie& Tie) { return Algo::Contains(Ties, (uint64)Tie.ID); }), ZipTies.end());
            NotifyCosmeticChange();
        }

        TVector<CEdGraphNode*> DeletedNodes;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            if (NodeRef.IsValid() && NodeRef->IsDeletable() && Editor.IsNodeSelected(NodeRef->GetNodeID()))
            {
                DeletedNodes.push_back(NodeRef.Get());
            }
        }

        if (!DeletedNodes.empty())
        {
            SetPendingEditLabel(DeletedNodes.size() == 1 ? FName("Delete Node") : FName("Delete Nodes"));
            RemoveNodes(DeletedNodes);
        }
    }

    void CEdNodeGraph::PasteClipboard(ImVec2 CanvasPos)
    {
        // The clipboard is shared, so filter to what this graph's registered node classes accept.
        const THashSet<CClass*>& Supported = GetSupportedNodes();
        TVector<CEdGraphNode*> Pastable;
        Pastable.reserve(GetNodeClipboard().size());
        uint32 Rejected = 0;
        for (CEdGraphNode* Copied : GetNodeClipboard())
        {
            if (Copied == nullptr)
            {
                continue;
            }

            if (Supported.find(Copied->GetClass()) != Supported.end())
            {
                Pastable.push_back(Copied);
            }
            else
            {
                ++Rejected;
            }
        }

        if (Rejected > 0)
        {
            ImGuiX::Notifications::NotifyWarning("Skipped {0} node(s) this graph does not support.", Rejected);
        }

        Editor.ClearSelection();
        SetPendingEditLabel("Paste Nodes");
        CloneNodes(Pastable, CanvasPos - GClipboardPivot);
    }

    void CEdNodeGraph::DrawCanvasPopups(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        if (ImGui::BeginPopup("Create New Node"))
        {
            DrawGraphContextMenu();
            ImGui::EndPopup();
        }
        else
        {
            PendingSourcePin = nullptr;
        }

        DrawFindPopup();
        DrawGeneratedCodePopup();

        if (ImGui::BeginPopup("Node Context Menu"))
        {
            DrawNodeContextMenu(FindNode(ContextMenuNodeID));
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("Pin Context Menu"))
        {
            if (CEdNodeGraphPin* Pin = FindPinByGUID(ContextMenuPinID))
            {
                ImGui::TextDisabled("%s", Pin->GetPinName().empty() ? "Pin" : Pin->GetPinName().c_str());
                ImGui::Separator();
                DrawPinContextMenu(Pin);
                if (ImGui::MenuItem(LE_ICON_LINK_OFF " Break Links", "Alt+Click", false, Pin->HasConnection()))
                {
                    SetPendingEditLabel("Break Links");
                    BreakPinLinks(Pin);
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("Link Context Menu"))
        {
            const ImVec2 OpenedAt = Editor.ScreenToCanvas(ImGui::GetMousePosOnOpeningCurrentPopup());
            if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* Link = FindLink(Links, ContextMenuLinkID))
            {
                if (ImGui::MenuItem(LE_ICON_LINK_OFF " Break Link", "Delete"))
                {
                    SetPendingEditLabel("Break Link");
                    BreakLink(Link->first, Link->second);
                }

                if (GetRerouteNodeClass() != nullptr && ImGui::MenuItem(LE_ICON_RAY_VERTEX " Insert Reroute", "Double-Click"))
                {
                    SetPendingEditLabel("Insert Reroute");
                    InsertRerouteOnLink(Link->second, Link->first, OpenedAt);
                }

                TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Selected;
                for (uint64 LinkID : Editor.GetSelectedLinks())
                {
                    if (const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>* SelectedLink = FindLink(Links, LinkID))
                    {
                        Selected.push_back(*SelectedLink);
                    }
                }
                if (Selected.empty())
                {
                    Selected.push_back(*Link);
                }

                if (ImGui::MenuItem(LE_ICON_CABLE_DATA " Zip Tie Selected Wires", "Alt+Right-Drag"))
                {
                    SetPendingEditLabel("Zip Tie Wires");
                    AddZipTie(Selected, OpenedAt);
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("Zip Tie Context Menu"))
        {
            if (ImGui::MenuItem(LE_ICON_CONTENT_CUT " Remove Zip Tie", "Delete"))
            {
                SetPendingEditLabel("Remove Zip Tie");
                ZipTies.erase(Algo::RemoveIf(ZipTies, [this](const SGraphZipTie& Tie) { return Tie.ID == ContextMenuTieID; }), ZipTies.end());
                NotifyCosmeticChange();
            }
            ImGui::EndPopup();
        }

        DrawChoicePopup();
    }

    void CEdNodeGraph::SubmitZipTies()
    {
        TVector<uint64> LinkIDs;
        for (SGraphZipTie& Tie : ZipTies)
        {
            LinkIDs.clear();
            for (size_t Pair = 0; Pair < Tie.Links.size() / 2; ++Pair)
            {
                LinkIDs.push_back(GetTiedLinkID(Tie, Pair));
            }

            ImVec2 Position(Tie.X, Tie.Y);
            Editor.ZipTie(Tie.ID, Position, LinkIDs);
            if (Position.x != Tie.X || Position.y != Tie.Y)
            {
                Tie.X = Position.x;
                Tie.Y = Position.y;
                bLayoutChangedSinceImage = true;
            }
        }
    }

    void CEdNodeGraph::AddZipTie(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links, ImVec2 CanvasPos)
    {
        if (Links.empty())
        {
            return;
        }

        SGraphZipTie NewTie;
        NewTie.X = CanvasPos.x;
        NewTie.Y = CanvasPos.y;

        for (const auto& [InputPin, OutputPin] : Links)
        {
            NewTie.Links.push_back(OutputPin->GetPinGUID());
            NewTie.Links.push_back(InputPin->GetPinGUID());
        }

        // A wire passes through every tie it is in, so the new tie leaves the others as they were.
        for (const SGraphZipTie& Tie : ZipTies)
        {
            NewTie.ID = Math::Max(NewTie.ID, Tie.ID);
        }
        ++NewTie.ID;
        ZipTies.push_back(Move(NewTie));
        NotifyCosmeticChange();
    }

    void CEdNodeGraph::PruneZipTies()
    {
        if (ZipTies.empty())
        {
            return;
        }

        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Links;
        CollectLinks(Links);
        THashSet<uint64> Live;
        for (const auto& [InputPin, OutputPin] : Links)
        {
            Live.insert(MakeLinkID(InputPin, OutputPin));
        }

        bool bChanged = false;
        for (SGraphZipTie& Tie : ZipTies)
        {
            bChanged |= RetainTiedLinks(Tie, [&Live](uint64 LinkID) { return Live.contains(LinkID); });
        }

        const size_t TieCount = ZipTies.size();
        ZipTies.erase(Algo::RemoveIf(ZipTies, [](const SGraphZipTie& Tie) { return Tie.Links.empty(); }), ZipTies.end());
        if (bChanged || TieCount != ZipTies.size())
        {
            NotifyCosmeticChange();
        }
    }


    void CEdNodeGraph::DrawGraphContextMenu()
    {
        if (ActionMenu.Draw(this))
        {
            ImGui::CloseCurrentPopup();
        }
    }

    void CEdNodeGraph::DrawNodeContextMenu(CEdGraphNode* Node)
    {
        if (Node != nullptr)
        {
            ImGui::TextDisabled("%s", FString(Node->GetNodeDisplayName()).c_str());
            ImGui::Separator();

            Node->DrawContextMenu();

            DrawNodeMenuItems(Node);

            FString Code;
            if (GetNodeGeneratedCode(Node, Code) && ImGui::MenuItem(LE_ICON_CODE_BRACES " Show Generated Code"))
            {
                CodeViewNodeID = Node->GetNodeID();
                bOpenCodeView  = true;
            }

            if (Node->IsCommentNode() && ImGui::MenuItem(LE_ICON_RENAME " Rename", "F2"))
            {
                BeginRenameNode(Node);
            }
        }

        if (ImGui::MenuItem(LE_ICON_COMMENT_TEXT_OUTLINE " Comment Selection", "C"))
        {
            WrapSelectionInComment();
        }

        if (ImGui::BeginMenu("Alignment"))
        {
            DrawAlignmentMenuItems();
            ImGui::EndMenu();
        }

        DrawGraphToolsMenuItems();
    }

    void CEdNodeGraph::DrawGraphToolsMenuItems()
    {
        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_AUTO_FIX " Tidy Graph"))
        {
            TidyGraph();
        }
        ImGuiX::TextTooltip("Lay the graph out in columns by distance from the output, ordered to reduce "
                            "crossing wires. Unused nodes are parked off to the left.");

        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);

        // A menu whose entries come and go is harder to learn than one that says there is nothing to do.
        ImGui::BeginDisabled(Dead.empty());
        if (ImGui::MenuItem(Dead.empty()
                ? LE_ICON_SELECTION_OFF " No Unused Nodes"
                : FormatAs<FFixedString>(LE_ICON_SELECTION " Select {} Unused Node{}",
                    (int32)Dead.size(), Dead.size() == 1 ? "" : "s").c_str()))
        {
            SelectDeadNodes();
        }

        if (ImGui::MenuItem(LE_ICON_BROOM " Delete Unused Nodes"))
        {
            DeleteDeadNodes();
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Nodes whose result never reaches the output. The compiler already ignores "
                            "them; this removes them from the canvas.");
    }

    void CEdNodeGraph::DrawPinContextMenu(CEdNodeGraphPin* Pin)
    {
    }

    void CEdNodeGraph::CollectClassesAccepting(CEdNodeGraphPin* SourcePin, THashSet<CClass*>& OutClasses)
    {
        for (CClass* NodeClass : GetSupportedNodes())
        {
            TStrongObjectPtr<CEdGraphNode> Probe(NewObject<CEdGraphNode>(NodeClass));
            Probe->OwningGraph = this;
            Probe->BuildNode();
            if (FindAutoConnectPin(Probe.Get(), SourcePin) != nullptr)
            {
                OutClasses.insert(NodeClass);
            }
        }
    }

    CEdGraphNode* CEdNodeGraph::CreateNode(CClass* NodeClass, FName Variant)
    {
        CEdGraphNode* NewNode = NewObject<CEdGraphNode>(NodeClass, GetNodeOuter());
        if (!Variant.IsNone())
        {
            NewNode->ApplyVariant(Variant);
        }
        AddNode(NewNode);
        return NewNode;
    }

    void CEdNodeGraph::HandleRenameShortcut()
    {
        // A second selected node makes the target ambiguous, so rename nothing.
        const bool bCanTakeKey = !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (bCanTakeKey && ImGui::IsKeyPressed(ImGuiKey_F2, false) && Editor.GetSelectedNodeCount() == 1)
        {
            BeginRenameNode(FindNode(Editor.GetSelectedNodes()[0]));
        }

        if (bOpenRenamePopup)
        {
            bOpenRenamePopup = false;
            ImGui::OpenPopup("Rename Node");
        }

        if (!ImGui::BeginPopup("Rename Node"))
        {
            return;
        }

        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(200.0f);
        const bool bCommitted = ImGui::InputText("##RenameNode", RenameBuffer, sizeof(RenameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        if (bCommitted)
        {
            if (CEdGraphNode* Node = FindNode(RenameNodeID))
            {
                Node->SetRenameText(FString(RenameBuffer));
                Node->NotifyValueEdited();
                GetPackage()->MarkDirty();
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void CEdNodeGraph::OpenChoicePopup(CEdGraphNode* Node, TVector<FString> InChoices, int32 Current, TFunction<void(int32)> OnChosen)
    {
        if (Node == nullptr || InChoices.empty())
        {
            return;
        }

        // The item rect is in canvas space here, and the popup opens in screen space after the canvas.
        const ImVec2 Min = Editor.CanvasToScreen(ImGui::GetItemRectMin());
        const ImVec2 Max = Editor.CanvasToScreen(ImGui::GetItemRectMax());

        ChoiceNodeID     = Node->GetNodeID();
        ChoiceCurrent    = Current;
        ChoiceAnchor     = ImVec2(Min.x, Max.y);
        ChoiceWidth      = Max.x - Min.x;
        Choices          = std::move(InChoices);
        OnChoiceChosen   = std::move(OnChosen);
        bOpenChoicePopup = true;
    }

    void CEdNodeGraph::DrawChoicePopup()
    {
        if (bOpenChoicePopup)
        {
            bOpenChoicePopup = false;
            ImGui::OpenPopup("##NodeChoice");
        }

        if (!ImGui::IsPopupOpen("##NodeChoice"))
        {
            return;
        }

        ImGui::SetNextWindowPos(ChoiceAnchor, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ChoiceWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (!ImGui::BeginPopup("##NodeChoice"))
        {
            return;
        }

        for (int32 Index = 0; Index < (int32)Choices.size(); ++Index)
        {
            if (ImGui::Selectable(Choices[Index].c_str(), Index == ChoiceCurrent))
            {
                if (FindNode(ChoiceNodeID) != nullptr && OnChoiceChosen)
                {
                    OnChoiceChosen(Index);
                }
            }
            if (Index == ChoiceCurrent && ImGui::IsWindowAppearing())
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndPopup();
    }

    void CEdNodeGraph::BeginRenameNode(CEdGraphNode* Node)
    {
        FString Current;
        if (Node == nullptr || !Node->GetRenameText(Current))
        {
            return;
        }

        RenameNodeID = Node->GetNodeID();
        const size_t Count = Math::Min(Current.size(), sizeof(RenameBuffer) - 1);
        Memory::Memcpy(RenameBuffer, Current.data(), Count);
        RenameBuffer[Count] = '\0';
        bOpenRenamePopup = true;
    }

    void CEdNodeGraph::HandleCommentShortcut()
    {
        const ImGuiIO& IO = ImGui::GetIO();
        const bool bCanTakeKey = !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt;

        if (bCanTakeKey && ImGui::IsKeyPressed(ImGuiKey_C, false) && Editor.GetSelectedNodeCount() > 0)
        {
            bCommentKeyArmed = true;
        }

        if (bCommentKeyArmed && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bCommentKeyArmed = false;
        }

        if (bCommentKeyArmed && ImGui::IsKeyReleased(ImGuiKey_C))
        {
            bCommentKeyArmed = false;
            if (bCanTakeKey)
            {
                WrapSelectionInComment();
            }
        }
    }

    CEdNode_Comment* CEdNodeGraph::WrapSelectionInComment()
    {
        ImVec2 BoundsMin(FLT_MAX, FLT_MAX);
        ImVec2 BoundsMax(-FLT_MAX, -FLT_MAX);
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            if (NodeRef.IsValid() && Editor.IsNodeSelected(NodeRef->GetNodeID()))
            {
                const ImRect Bounds = GetNodeBounds(NodeRef.Get());
                BoundsMin = ImMin(BoundsMin, Bounds.Min);
                BoundsMax = ImMax(BoundsMax, Bounds.Max);
            }
        }

        const CEdNode_Comment* Defaults = CEdNode_Comment::StaticClass()->GetDefaultObject<CEdNode_Comment>();
        ImVec2 Position = Editor.ScreenToCanvas(ImGui::GetMousePos());
        ImVec2 Size(Defaults->Width, Defaults->Height);
        if (BoundsMin.x <= BoundsMax.x)
        {
            Position = ImVec2(BoundsMin.x - kCommentWrapMargin, BoundsMin.y - kCommentWrapMargin - kCommentHeaderHeight);
            Size     = BoundsMax - BoundsMin + ImVec2(kCommentWrapMargin * 2.0f, kCommentWrapMargin * 2.0f);
        }

        SetPendingEditLabel("Add Comment");
        CEdNode_Comment* Comment = Cast<CEdNode_Comment>(SpawnNode(CEdNode_Comment::StaticClass(), Position.x, Position.y));
        if (Comment == nullptr)
        {
            return nullptr;
        }

        Comment->Width  = Math::Max(Size.x, kCommentMinWidth);
        Comment->Height = Math::Max(Size.y, kCommentMinHeight);

        // Named straight away, since a comment's whole point is its text.
        BeginRenameNode(Comment);
        return Comment;
    }

    void CEdNodeGraph::DrawCommentNode(CEdNode_Comment* Comment)
    {
        using namespace ImGuiX::Font;

        const int64 ID = Comment->GetNodeID();
        ImVec2 BodySize(Math::Max(Comment->Width, kCommentMinWidth), Math::Max(Comment->Height, kCommentMinHeight));

        // The description lives in the title bar, which grows to fit it, so the nodes inside never cover it.
        TVector<FString> DescriptionLines;
        float DescriptionLineHeight = 0.0f;
        float HeaderHeight = kCommentHeaderHeight;
        if (!Comment->Description.empty())
        {
            PushFont(EFont::Small);
            DescriptionLineHeight = ImGui::GetTextLineHeight();
            WrapWords(ImGui::GetFont(), ImGui::GetFontSize(), Comment->Description, BodySize.x - kCommentPadding * 2.0f, DescriptionLines);
            PopFont();
            HeaderHeight += DescriptionLineHeight * (float)DescriptionLines.size() + kCommentPadding * 0.5f;
        }

        ImVec2 Position(Comment->GetNodeX(), Comment->GetNodeY());
        Editor.Comment(ID, Position, BodySize, HeaderHeight, ImVec2(kCommentMinWidth, kCommentMinHeight));
        SyncNodePosition(Comment, Position);
        if (BodySize.x != Comment->Width || BodySize.y != Comment->Height)
        {
            Comment->Width  = BodySize.x;
            Comment->Height = BodySize.y;
            bLayoutChangedSinceImage = true;
        }

        ImDrawList* DrawList = Editor.GetNodeBackgroundDrawList(ID);
        if (DrawList == nullptr)
        {
            return;
        }

        const ImVec2 Min       = Position;
        const ImVec2 Max       = Min + ImVec2(BodySize.x, HeaderHeight + BodySize.y);
        const ImVec2 HeaderMax = ImVec2(Max.x, Min.y + HeaderHeight);
        const bool   bSelected = Editor.IsNodeSelected(ID);
        const bool   bHovered  = Editor.GetHoveredNode() == ID;

        DrawList->AddRectFilled(Min + ImVec2(0.0f, 3.0f), Max + ImVec2(0.0f, 6.0f), IM_COL32(0, 0, 0, 38), kCommentRounding + 2.0f);
        DrawList->AddRectFilled(Min, Max, Comment->GetTint(bSelected ? 0.20f : 0.13f), kCommentRounding);
        DrawList->AddRectFilled(Min, HeaderMax, Comment->GetTint(0.95f), kCommentRounding, ImDrawFlags_RoundCornersTop);
        DrawList->AddRectFilled(Min, ImVec2(HeaderMax.x, Min.y + kCommentHeaderHeight * 0.5f), IM_COL32(255, 255, 255, 22), kCommentRounding, ImDrawFlags_RoundCornersTop);
        DrawList->AddLine(ImVec2(Min.x + 1.0f, HeaderMax.y), ImVec2(HeaderMax.x - 1.0f, HeaderMax.y), IM_COL32(0, 0, 0, 80), 1.0f);
        DrawList->AddRect(Min, Max, Comment->GetTint(bHovered || bSelected ? 1.0f : 0.7f), kCommentRounding, bSelected ? 2.5f : 1.5f);

        const ImU32 TitleColor = Comment->GetTitleTextColor();
        DrawList->PushClipRect(Min, HeaderMax, true);
        {
            PushFont(EFont::MediumBold);
            const float  FontSize = ImGui::GetFontSize();
            const float  TextY    = Min.y + (kCommentHeaderHeight - FontSize) * 0.5f;
            const ImVec2 IconPos(Min.x + kCommentPadding, TextY);
            DrawList->AddText(ImGui::GetFont(), FontSize, IconPos, ScaleCommentAlpha(TitleColor, 0.85f), LE_ICON_COMMENT_TEXT_OUTLINE);
            const float TitleX = IconPos.x + ImGui::CalcTextSize(LE_ICON_COMMENT_TEXT_OUTLINE).x + 8.0f;
            AddTextEllipsized(DrawList, ImGui::GetFont(), FontSize, ImVec2(TitleX, TextY), TitleColor, Comment->Title, Max.x - kCommentPadding - TitleX);
            PopFont();

            if (!DescriptionLines.empty())
            {
                PushFont(EFont::Small);
                const ImU32 DescriptionColor = ScaleCommentAlpha(TitleColor, 0.78f);
                ImVec2 LinePos(Min.x + kCommentPadding, Min.y + kCommentHeaderHeight - 6.0f);
                for (const FString& Line : DescriptionLines)
                {
                    DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), LinePos, DescriptionColor, Line.c_str(), Line.c_str() + Line.size());
                    LinePos.y += DescriptionLineHeight;
                }
                PopFont();
            }
        }
        DrawList->PopClipRect();

        // Three short strokes in the corner say the box resizes from there.
        const ImU32 GripColor = Comment->GetTint(bHovered ? 1.0f : 0.6f);
        for (int32 Stroke = 1; Stroke <= 3; ++Stroke)
        {
            const float Offset = 4.0f * (float)Stroke;
            DrawList->AddLine(ImVec2(Max.x - 4.0f - Offset, Max.y - 4.0f), ImVec2(Max.x - 4.0f, Max.y - 4.0f - Offset), GripColor, 1.5f);
        }
    }

    void CEdNodeGraph::DrawCommentHints()
    {
        using namespace ImGuiX::Font;

        // Once the canvas is too far out to read a comment's own title, it is restated at screen size over the comment.
        const float Fade = Math::Saturate((0.65f - Editor.GetZoom()) / 0.25f);
        if (Fade <= 0.0f)
        {
            return;
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        PushFont(EFont::LargeBold);
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            if (!NodeRef.IsValid() || !NodeRef->IsCommentNode())
            {
                continue;
            }

            const CEdNode_Comment* Comment = static_cast<const CEdNode_Comment*>(NodeRef.Get());
            const ImRect Bounds = GetNodeBounds(Comment);
            const ImVec2 Min    = Editor.CanvasToScreen(Bounds.Min);
            const ImVec2 Max    = Editor.CanvasToScreen(Bounds.Max);
            DrawList->AddRect(Min, Max, Comment->GetTint(0.9f * Fade), 4.0f, 2.0f);

            const FString& Title    = Comment->Title;
            const ImVec2   TextSize = ImGui::CalcTextSize(Title.c_str(), Title.c_str() + Title.size());

            // Laid over the comment's own title bar, so it can never land on a neighboring comment.
            const ImVec2 BarMax(Max.x, Min.y + TextSize.y + 12.0f);
            DrawList->AddRectFilled(Min, BarMax, Comment->GetTint(Fade), 4.0f, ImDrawFlags_RoundCornersTop);
            DrawList->PushClipRect(Min, BarMax, true);
            DrawList->AddText(ImVec2(Min.x + 12.0f, Min.y + 6.0f), ScaleCommentAlpha(Comment->GetTitleTextColor(), Fade), Title.c_str(), Title.c_str() + Title.size());
            DrawList->PopClipRect();
        }
        PopFont();
    }

    CEdGraphNode* CEdNodeGraph::FindNode(int64 InNodeID) const
    {
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid() && Node->GetNodeID() == InNodeID)
            {
                return Node.Get();
            }
        }
        return nullptr;
    }
    
    const THashSet<CClass*>& CEdNodeGraph::GetSupportedNodes()
    {
        FGraphNodeRegistry& Registry = FGraphNodeRegistry::Get();

        // So a graph left open across a plugin load picks up that plugin's nodes.
        if (!bSupportedNodesBuilt || SupportedNodesGeneration != Registry.GetGeneration())
        {
            bSupportedNodesBuilt    = true;
            SupportedNodesGeneration = Registry.GetGeneration();

            // A union, since RegisterGraphNode adds per-instance entries the shared cache knows nothing about.
            const THashSet<CClass*>& Discovered = Registry.GetNodesForGraphClass(GetClass());
            SupportedNodes.insert(Discovered.begin(), Discovered.end());
        }

        return SupportedNodes;
    }

    void CEdNodeGraph::RegisterGraphNode(CClass* InClass)
    {
        if (SupportedNodes.find(InClass) == SupportedNodes.end())
        {
            SupportedNodes.emplace(InClass);
        }
    }

    CEdNodeGraphPin* CEdNodeGraph::FindAutoConnectPin(CEdGraphNode* NewNode, CEdNodeGraphPin* SourcePin) const
    {
        if (NewNode == nullptr || SourcePin == nullptr)
        {
            return nullptr;
        }

        const FEdGraphSchema& Schema = GetSchema();
        const bool bSourceIsInput = SourcePin->bInputPin;
        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Candidates = bSourceIsInput
            ? NewNode->GetOutputPins()
            : NewNode->GetInputPins();

        for (const TStrongObjectPtr<CEdNodeGraphPin>& Candidate : Candidates)
        {
            if (!Candidate.IsValid() || Candidate->IsDisabled())
            {
                continue;
            }

            CEdNodeGraphPin* From = bSourceIsInput ? Candidate.Get() : SourcePin;
            CEdNodeGraphPin* To   = bSourceIsInput ? SourcePin : Candidate.Get();
            if (Schema.CanCreateConnection(From, To))
            {
                return Candidate.Get();
            }
        }

        return nullptr;
    }

    void CEdNodeGraph::TryAutoConnect(CEdNodeGraphPin* SourcePin, CEdNodeGraphPin* TargetPin)
    {
        if (SourcePin == nullptr || TargetPin == nullptr)
        {
            return;
        }

        const bool bSourceIsInput = SourcePin->bInputPin;
        ConnectPins(bSourceIsInput ? TargetPin : SourcePin, bSourceIsInput ? SourcePin : TargetPin);
    }

    int64 CEdNodeGraph::GenerateUniqueNodeID(int64 PreferredID) const
    {
        const auto IsTaken = [this](int64 Candidate)
        {
            for (const TStrongObjectPtr<CEdGraphNode>& Existing : Nodes)
            {
                if (Existing.IsValid() && Existing->GetNodeID() == Candidate)
                {
                    return true;
                }
            }
            return false;
        };

        // A deserialized node keeps its ID so links and saved positions still resolve.
        if (PreferredID != 0 && !IsTaken(PreferredID))
        {
            return PreferredID;
        }

        // 0 is the unassigned sentinel AddNode tests against, so a node holding it would be re-rolled.
        for (int32 Attempt = 0; Attempt < 64; ++Attempt)
        {
            const int64 Candidate = (int64)Math::RandRange(1u, UINT32_MAX);
            if (!IsTaken(Candidate))
            {
                return Candidate;
            }
        }

        // Unreachable in practice against a 32-bit space, but a scan cannot fail where a roll can.
        int64 Sequential = 1;
        while (IsTaken(Sequential))
        {
            ++Sequential;
        }

        return Sequential;
    }

    uint64 CEdNodeGraph::AddNode(CEdGraphNode* InNode)
    {
        // A duplicate ID silently merges two nodes' interaction state rather than failing outright.
        const int64 NodeID = GenerateUniqueNodeID(InNode->NodeID);

        if (InNode->NodeID != 0 && NodeID != InNode->NodeID)
        {
            LOG_WARN("Node graph: '{}' was added with an already-used node ID {}; reassigned to {}. "
                     "Saved links referencing the old ID will not resolve.",
                     InNode->GetNodeDisplayName(), InNode->NodeID, NodeID);
        }

        InNode->PinHashName = FString(InNode->GetNodeDisplayName()) + "_" + Format("{}", NodeID);
        InNode->FullName = SanitizeNodeIdentifier(InNode->PinHashName);
        InNode->NodeID = NodeID;
        InNode->OwningGraph = this;

        Nodes.push_back(InNode);

        if (!InNode->bWasBuild)
        {
            InNode->BuildNode();
            InNode->bWasBuild = true;
        }

        NotifyContentChanged();
        if (!bIsPostLoading)
        {
            ValidateGraph();
        }

        return NodeID;
    }
    

    void CEdNodeGraph::SetTransactionManager(FTransactionManager* InManager)
    {
        TransactionManager = InManager;
        UndoImage = FNodeGraphImage();
        RebaseUndoImage();
    }

    void CEdNodeGraph::RebaseUndoImage()
    {
        PendingEditLabel = FName();
        bLayoutChangedSinceImage = false;
        UndoImageContentVersion = ContentVersion;

        if (TransactionManager != nullptr)
        {
            UndoImage = FNodeGraphImage::Capture(this);
        }
    }

    void CEdNodeGraph::CommitEdit(FName Label)
    {
        if (TransactionManager == nullptr)
        {
            return;
        }

        // A change made while a step is being applied belongs to that step, so it only moves the baseline.
        if (!UndoImage.IsValid() || TransactionManager->IsApplying())
        {
            RebaseUndoImage();
            return;
        }

        FNodeGraphImage Current = FNodeGraphImage::Capture(this);
        if (Current.Bytes == UndoImage.Bytes)
        {
            UndoImageContentVersion = ContentVersion;
            bLayoutChangedSinceImage = false;
            PendingEditLabel = FName();
            return;
        }

        const FName Name = !Label.IsNone() ? Label : !PendingEditLabel.IsNone() ? PendingEditLabel : FName("Edit Graph");
        TUniquePtr<IUndoableCommand> Command = MakeUnique<FNodeGraphSnapshotCommand>(this, Move(UndoImage), Current);

        // Joins an edit already being recorded, such as a property change spanning the graph and its asset.
        if (TransactionManager->IsRecording())
        {
            TransactionManager->Record(Move(Command));
        }
        else
        {
            TransactionManager->BeginTransaction(Name);
            TransactionManager->Record(Move(Command));
            TransactionManager->CommitTransaction();
        }

        UndoImage = Move(Current);
        UndoImageContentVersion = ContentVersion;
        bLayoutChangedSinceImage = false;
        PendingEditLabel = FName();
    }

    void CEdNodeGraph::CommitEditIntoLastStep()
    {
        if (TransactionManager == nullptr || TransactionManager->IsApplying() || !UndoImage.IsValid())
        {
            RebaseUndoImage();
            return;
        }

        FNodeGraphImage Current = FNodeGraphImage::Capture(this);
        if (Current.Bytes != UndoImage.Bytes)
        {
            TransactionManager->AppendToLast(MakeUnique<FNodeGraphSnapshotCommand>(this, Move(UndoImage), Current));
        }

        UndoImage = Move(Current);
        UndoImageContentVersion = ContentVersion;
        bLayoutChangedSinceImage = false;
    }

    void CEdNodeGraph::LeaveEditScope(FName Label)
    {
        if (EditScopeDepth > 0 && --EditScopeDepth == 0)
        {
            CommitEdit(Label);
        }
    }

    void CEdNodeGraph::BeginLongEdit()
    {
        ++LongEditDepth;
    }

    void CEdNodeGraph::EndLongEdit(FName Label)
    {
        if (LongEditDepth > 0 && --LongEditDepth == 0)
        {
            CommitEdit(Label);
        }
    }

    void CEdNodeGraph::CommitFrameEdits()
    {
        if (TransactionManager == nullptr || EditScopeDepth > 0 || LongEditDepth > 0)
        {
            return;
        }

        const bool bContentChanged = ContentVersion != UndoImageContentVersion;
        if (!bContentChanged && !bLayoutChangedSinceImage)
        {
            return;
        }

        // A drag, a wire being pulled or an inline value being scrubbed is one step, taken when it ends.
        if (ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        {
            return;
        }

        if (!bContentChanged && PendingEditLabel.IsNone())
        {
            PendingEditLabel = "Move Nodes";
        }
        CommitEdit(FName());
    }

    CEdGraphNode* CEdNodeGraph::SpawnNode(CClass* NodeClass, float X, float Y, FName Variant)
    {
        if (NodeClass == nullptr)
        {
            return nullptr;
        }

        CEdGraphNode* Node = CreateNode(NodeClass, Variant);
        if (Node != nullptr)
        {
            Node->SetGridPos(X, Y);
        }

        return Node;
    }

    bool CEdNodeGraph::RemoveNodes(const TVector<CEdGraphNode*>& InNodes, FString* OutError)
    {
        for (CEdGraphNode* Node : InNodes)
        {
            const bool bInGraph = Node != nullptr
                && Algo::FindIf(Nodes, [Node](const TStrongObjectPtr<CEdGraphNode>& N) { return N.Get() == Node; }) != Nodes.end();
            if (!bInGraph)
            {
                if (OutError)
                {
                    *OutError = "That node is not in this graph.";
                }
                return false;
            }

            if (!Node->IsDeletable())
            {
                if (OutError)
                {
                    *OutError = Format("{} belongs to the graph itself, so it cannot be removed.", Node->GetNodeDisplayName());
                }
                return false;
            }
        }

        for (CEdGraphNode* Node : InNodes)
        {
            // Held until the loop is done with it, since erasing drops what may be the last reference.
            TStrongObjectPtr<CEdGraphNode> Keep = Node;

            if (PreNodeDeletedCallback)
            {
                PreNodeDeletedCallback(Node);
            }

            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Node->GetInputPins())
            {
                Pin->ClearConnections();
            }
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Node->GetOutputPins())
            {
                Pin->ClearConnections();
            }

            // The copy buffer holds raw pointers, so a node deleted between copy and paste would be cloned dead.
            ForgetClipboardNode(Node);

            Nodes.erase(Algo::RemoveIf(Nodes, [Node](const TStrongObjectPtr<CEdGraphNode>& N) { return N.Get() == Node; }), Nodes.end());
        }

        NotifyContentChanged();
        PruneZipTies();
        ValidateGraph();
        return true;
    }

    bool CEdNodeGraph::ConnectPins(CEdNodeGraphPin* Output, CEdNodeGraphPin* Input, FString* OutError)
    {
        auto Fail = [OutError](const FString& Message)
        {
            if (OutError)
            {
                *OutError = Message;
            }
            return false;
        };

        if (Output == nullptr || Input == nullptr)
        {
            return Fail("A connection needs both pins.");
        }

        if (Output->bInputPin || !Input->bInputPin)
        {
            return Fail("A connection runs from an output pin to an input pin.");
        }

        if (Output->IsDisabled() || Input->IsDisabled())
        {
            return Fail("One of those pins is disabled.");
        }

        const FEdGraphSchema& Schema = GetSchema();
        if (!Schema.CanCreateConnection(Output, Input))
        {
            return Fail(Format("'{}' and '{}' cannot be connected, since the graph refuses that pairing.",
                Output->GetPinName(), Input->GetPinName()));
        }

        const TVector<CEdNodeGraphPin*> Existing = Input->GetConnections();
        if (Algo::Find(Existing, Output) != Existing.end())
        {
            return true;
        }

        // An input holding a second link is unrepresentable in the editor and loses one on reload.
        if (Input->HasConnection() && !Schema.AllowsMultipleConnections(Input))
        {
            Input->ClearConnections();
        }

        Output->AddConnection(Input);
        Input->AddConnection(Output);

        NotifyContentChanged();
        PruneZipTies();
        ValidateGraph();
        return true;
    }

    void CEdNodeGraph::BreakPinLinks(CEdNodeGraphPin* Pin)
    {
        if (Pin == nullptr || !Pin->HasConnection())
        {
            return;
        }

        Pin->ClearConnections();
        NotifyContentChanged();
        PruneZipTies();
        ValidateGraph();
    }

    void CEdNodeGraph::BreakLink(CEdNodeGraphPin* PinA, CEdNodeGraphPin* PinB)
    {
        if (PinA == nullptr || PinB == nullptr)
        {
            return;
        }

        PinA->RemoveConnection(PinB);
        PinB->RemoveConnection(PinA);
        NotifyContentChanged();
        PruneZipTies();
        ValidateGraph();
    }

    void CEdNodeGraph::MoveNode(CEdGraphNode* Node, float X, float Y)
    {
        if (Node == nullptr)
        {
            return;
        }

        Node->SetGridPos(X, Y);
        bLayoutChangedSinceImage = true;
    }

    bool CEdNodeGraph::GetVisibleCanvasRect(ImVec2& OutMin, ImVec2& OutMax, float& OutZoom) const
    {
        const ImRect Visible = Editor.GetVisibleCanvasRect();
        OutMin  = Visible.Min;
        OutMax  = Visible.Max;
        OutZoom = Editor.GetZoom();
        return Editor.HasDrawn();
    }

    void CEdNodeGraph::CommitExternalNodeEdit(FName Label)
    {
        NotifyContentChanged();
        ValidateGraph();
        CommitEdit(Label);
    }

    void CEdNodeGraph::FixupAfterRestore(const TVector<CEdGraphNode*>& PreviousNodes)
    {
        for (CEdGraphNode* Node : PreviousNodes)
        {
            const bool bStillPresent = Algo::FindIf(Nodes, [Node](const TStrongObjectPtr<CEdGraphNode>& N) { return N.Get() == Node; }) != Nodes.end();
            if (Node != nullptr && !bStillPresent)
            {
                if (PreNodeDeletedCallback)
                {
                    PreNodeDeletedCallback(Node);
                }
                ForgetClipboardNode(Node);
            }
        }

        // Links live on pins as raw pointers, so they are dropped everywhere and rebuilt from Connections.
        auto ClearPins = [](CEdGraphNode* Node)
        {
            if (Node == nullptr)
            {
                return;
            }
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Node->GetInputPins())
            {
                Pin->ClearConnections();
            }
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Node->GetOutputPins())
            {
                Pin->ClearConnections();
            }
        };
        for (CEdGraphNode* Node : PreviousNodes)
        {
            ClearPins(Node);
        }
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            ClearPins(Node.Get());
            if (Node.IsValid())
            {
                Node->OwningGraph = this;
            }
        }

        RelinkFromConnections();

        NotifyContentChanged();
        ValidateGraph();
    }
}
