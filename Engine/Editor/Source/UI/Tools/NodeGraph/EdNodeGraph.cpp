#include "EdNodeGraph.h"

#include "EdGraphNode.h"
#include "EdNode_Reroute.h"
#include "GraphAlgorithms.h"
#include "GraphNodeRegistry.h"
#include "Core/Object/Class.h"
#include <Core/Reflection/Type/LuminaTypes.h>
#include "Drawing.h"
#include "imgui_internal.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "imgui-node-editor/imgui_node_editor_internal.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"
#include "Core/Templates/IntegerCompare.h"

namespace Lumina
{
    namespace
    {
        // Raw pointers deliberately, since a TObjectPtr clipboard would outlive its graph at teardown.
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
            Clipboard.erase(Algo::Remove(Clipboard.begin(), Clipboard.end(), Node), Clipboard.end());
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

    static void DrawPinIcon(bool bConnected, int Alpha, ImVec4 Color)
    {
        EIconType iconType = EIconType::Circle;
        Color.w = Alpha / 255.0f;
        
        Icon(ImVec2(24.f, 24.0f), iconType, bConnected, Color, ImColor(32, 32, 32, Alpha));
    }
    
    CEdNodeGraph::CEdNodeGraph()
    {
    }

    CEdNodeGraph::~CEdNodeGraph()
    {
    }

    bool CEdNodeGraph::GraphSaveSettings(const char* data, size_t size, ax::NodeEditor::SaveReasonFlags reason, void* userPointer)
    {
        CEdNodeGraph* ThisGraph = (CEdNodeGraph*)userPointer;
        
        if (reason != ax::NodeEditor::SaveReasonFlags::None && !ThisGraph->bFirstDraw)
        {
            ThisGraph->GetPackage()->MarkDirty();
            ThisGraph->GraphSaveData.assign(data, size);
        }
        
        return true;
    }
    
    size_t CEdNodeGraph::GraphLoadSettings(char* Data, void* UserPointer)
    {
        CEdNodeGraph* ThisGraph = (CEdNodeGraph*)UserPointer;
        if (Data)
        {
            Memory::Memcpy(Data, ThisGraph->GraphSaveData.data(), ThisGraph->GraphSaveData.size());
        }
        return ThisGraph->GraphSaveData.size();
    }
    

    void CEdNodeGraph::Initialize()
    {
        ax::NodeEditor::Config Config;
        Config.EnableSmoothZoom = true;
        Config.UserPointer = this;
        Config.SaveSettings = GraphSaveSettings;
        Config.LoadSettings = GraphLoadSettings;
        Config.SettingsFile = nullptr;
        Context = ax::NodeEditor::CreateEditor(&Config);
    }

    void CEdNodeGraph::Shutdown()
    {
        // Otherwise closing the editor you copied from leaves a paste pointing at freed nodes.
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid())
            {
                ForgetClipboardNode(Node.Get());
            }
        }

        ax::NodeEditor::DestroyEditor(Context);
        Context = nullptr;
    }

    void CEdNodeGraph::Serialize(FArchive& Ar)
    {
        Super::Serialize(Ar);
    }

    void CEdNodeGraph::PostLoad()
    {
        Super::PostLoad();
        
        TVector<TObjectPtr<CEdGraphNode>> SavedNodes = Move(Nodes);
        TVector<uint32> SavedConnections = Move(Connections);
        Nodes.clear();
        Connections.clear();

        // Reconciling before the links are back would drop everything keyed off them.
        bIsPostLoading = true;

        for (const TObjectPtr<CEdGraphNode>& Node : SavedNodes)
        {
            if (Node.IsValid())
            {
                AddNode(Node.Get());
            }
        }

        for (size_t i = 0; i + 1 < SavedConnections.size(); i += 2)
        {
            uint32 InputID = SavedConnections[i];
            uint32 OutputID = SavedConnections[i + 1];

            CEdNodeGraphPin* InputPin = nullptr;
            CEdNodeGraphPin* OutputPin = nullptr;

            for (CEdGraphNode* Node : Nodes)
            {
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

        // The load's one reconcile, rebuilding Connections and re-matching link-keyed data against them.
        bIsPostLoading = false;
        ValidateGraph();
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

    void CEdNodeGraph::DrawRerouteNode(CEdGraphNode* Node)
    {
        using namespace ax;

        // Two pins share one visual dot via PivotAlignment + zero radius.
        constexpr float DotRadius = 6.0f;
        constexpr ImU32 DotColor  = IM_COL32(220, 220, 220, 255);
        constexpr ImU32 DotShadow = IM_COL32(  0,   0,   0, 180);

        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodePadding,    ImVec4(0, 0, 0, 0));
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, 0.0f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeRounding,    0.0f);
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg,     ImColor(0, 0, 0, 0));
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, ImColor(0, 0, 0, 0));

        NodeEditor::BeginNode(Node->GetNodeID());
        ImGui::PushID((int)Node->GetNodeID());

        const ImVec2 CursorStart = ImGui::GetCursorScreenPos();
        const ImVec2 DotSize(DotRadius * 2.0f, DotRadius * 2.0f);

        // The hit area extends past the dot, so dragging a wire onto it hits a pin, not the node body.
        ImGui::Dummy(DotSize);

        const ImVec2 Center      = CursorStart + DotSize * 0.5f;
        const float  HitHalfX    = DotRadius * 2.0f;
        const ImVec2 InputRectMin (CursorStart.x - HitHalfX, CursorStart.y - DotRadius * 0.5f);
        const ImVec2 InputRectMax (Center.x,                  CursorStart.y + DotSize.y + DotRadius * 0.5f);
        const ImVec2 OutputRectMin(Center.x,                  InputRectMin.y);
        const ImVec2 OutputRectMax(CursorStart.x + DotSize.x + HitHalfX, InputRectMax.y);

        // Both pins share the same center pivot, so wires visually meet at a single dot.
        if (CEdNodeGraphPin* InputPin = Node->GetInputPins().empty() ? nullptr : Node->GetInputPins()[0].Get())
        {
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PivotAlignment, ImVec2(0.5f, 0.5f));
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PivotSize,      ImVec2(0, 0));
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PinRadius,      DotRadius);
            NodeEditor::BeginPin(InputPin->GetPinGUID(), NodeEditor::PinKind::Input);
            NodeEditor::PinPivotRect(Center, Center);
            NodeEditor::PinRect(InputRectMin, InputRectMax);
            NodeEditor::EndPin();
            NodeEditor::PopStyleVar(3);
        }

        // The right half of the same hit area, sharing the pivot so the visual dot stays one point.
        if (CEdNodeGraphPin* OutputPin = Node->GetOutputPins().empty() ? nullptr : Node->GetOutputPins()[0].Get())
        {
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PivotAlignment, ImVec2(0.5f, 0.5f));
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PivotSize,      ImVec2(0, 0));
            NodeEditor::PushStyleVar(NodeEditor::StyleVar_PinRadius,      DotRadius);
            NodeEditor::BeginPin(OutputPin->GetPinGUID(), NodeEditor::PinKind::Output);
            NodeEditor::PinPivotRect(Center, Center);
            NodeEditor::PinRect(OutputRectMin, OutputRectMax);
            NodeEditor::EndPin();
            NodeEditor::PopStyleVar(3);
        }

        ImGui::PopID();
        NodeEditor::EndNode();

        NodeEditor::PopStyleColor(2);
        NodeEditor::PopStyleVar(3);

        // Painted on the node background list, so it sits behind wires but above the node background.
        if (ImDrawList* DrawList = NodeEditor::GetNodeBackgroundDrawList(Node->GetNodeID()))
        {
            DrawList->AddCircleFilled(Center, DotRadius + 1.5f, DotShadow, 16);
            DrawList->AddCircleFilled(Center, DotRadius,        DotColor,  16);
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

        ax::NodeEditor::SetNodePosition(RerouteNode->GetNodeID(), CanvasPos);
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

        const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
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
        // Walking INPUT pins only visits each link once, so nothing gets connected twice.
        for (CEdGraphNode* Source : SourceOrder)
        {
            const auto CloneItr = Clones.find(Source);
            if (CloneItr == Clones.end())
            {
                continue;
            }

            for (const TObjectPtr<CEdNodeGraphPin>& SourceInput : Source->GetInputPins())
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

    void CEdNodeGraph::CloneNodes(const TVector<CEdGraphNode*>& SourceNodes, ImVec2 Delta)
    {
        using namespace ax;

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

            NodeEditor::SetNodePosition(Clone->GetNodeID(), NodeEditor::GetNodePosition(Source->GetNodeID()) + Delta);
            NodeEditor::SelectNode(Clone->GetNodeID(), true);
        }

        RelinkClones(SourceNodes, Clones);

        ValidateGraph();
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

        for (const TObjectPtr<CEdGraphNode>& SourceNode : Source->Nodes)
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

    uint32 CEdNodeGraph::UnaliasSubGraphs(THashSet<CEdNodeGraph*>& Visited)
    {
        uint32 Repaired = 0;

        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
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
        using namespace ax;

        // GetSelectedObjectCount counts links too, so it is an upper bound on the node count.
        const int32 SelectionBound = NodeEditor::GetSelectedObjectCount();
        if (SelectionBound < 2)
        {
            return;
        }

        TVector<NodeEditor::NodeId> Selected;
        Selected.resize(SelectionBound);
        const int32 Count = NodeEditor::GetSelectedNodes(Selected.data(), SelectionBound);
        if (Count < 2)
        {
            return;
        }

        struct FEntry
        {
            NodeEditor::NodeId Id;
            ImVec2             Pos;
            ImVec2             Size;
        };

        TVector<FEntry> Entries;
        Entries.reserve(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Entries.push_back(FEntry{ Selected[i], NodeEditor::GetNodePosition(Selected[i]), NodeEditor::GetNodeSize(Selected[i]) });
        }

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
        const bool bDistributeX = Alignment == ENodeAlignment::DistributeX;
        const bool bDistributeY = Alignment == ENodeAlignment::DistributeY;
        if (bDistributeX || bDistributeY)
        {
            if (Count < 3)
            {
                return;   // two nodes are already evenly spaced; nothing to solve
            }

            Algo::Sort(Entries.begin(), Entries.end(), [bDistributeX](const FEntry& A, const FEntry& B)
            {
                return bDistributeX ? (A.Pos.x < B.Pos.x) : (A.Pos.y < B.Pos.y);
            });

            const float Span    = bDistributeX ? (MaxRight - MinLeft) : (MaxBottom - MinTop);
            const float Used    = bDistributeX ? SumWidth : SumHeight;
            const float Gap     = (Span - Used) / (float)(Count - 1);
            float       Cursor  = bDistributeX ? MinLeft : MinTop;

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
                NodeEditor::SetNodePosition(Entry.Id, Entry.Pos);
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
            NodeEditor::SetNodePosition(Entry.Id, Entry.Pos);
        }
    }

    void CEdNodeGraph::CollectContributingNodes(THashSet<CEdGraphNode*>& OutContributing) const
    {
        for (CEdGraphNode* Node : Nodes)
        {
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

        for (CEdGraphNode* Node : Nodes)
        {
            if (Node != nullptr && Contributing.find(Node) == Contributing.end())
            {
                OutDead.push_back(Node);
            }
        }
    }

    void CEdNodeGraph::SelectDeadNodes()
    {
        using namespace ax;

        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);

        NodeEditor::ClearSelection();
        for (CEdGraphNode* Node : Dead)
        {
            NodeEditor::SelectNode(Node->GetNodeID(), /*append*/ true);
        }
    }

    void CEdNodeGraph::DeleteDeadNodes()
    {
        using namespace ax;

        TVector<CEdGraphNode*> Dead;
        CollectDeadNodes(Dead);

        // Routed through the editor's delete request, which already unhooks pins and re-validates.
        for (CEdGraphNode* Node : Dead)
        {
            NodeEditor::DeleteNode(Node->GetNodeID());
        }
    }

    void CEdNodeGraph::TidyGraph()
    {
        using namespace ax;

        // Column spacing is generous since cramped columns make long wires ambiguous.
        constexpr float kColumnGap = 90.0f;
        constexpr float kRowGap    = 28.0f;

        THashSet<CEdGraphNode*> Contributing;
        CollectContributingNodes(Contributing);
        if (Contributing.empty())
        {
            return;
        }

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
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
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

        const ImVec2 Origin = NodeEditor::GetNodePosition(Sorted.empty() ? Nodes[0]->GetNodeID() : Sorted.back()->GetNodeID());

        THashMap<CEdGraphNode*, float> PlacedY;

        // Roots are the rightmost column, so a column is ordered against consumers with final Y.
        float ColumnRight = Origin.x;
        for (int32 D = 0; D <= MaxDepth; ++D)
        {
            TVector<CEdGraphNode*>& Column = Columns[D];

            Algo::StableSort(Column.begin(), Column.end(), [&](CEdGraphNode* A, CEdGraphNode* B)
            {
                auto Barycenter = [&](CEdGraphNode* Node)
                {
                    float Sum = 0.0f;
                    int32 Count = 0;
                    for (CEdNodeGraphPin* OutputPin : Node->GetOutputPins())
                    {
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
                WidestInColumn = Math::Max(WidestInColumn, NodeEditor::GetNodeSize(Node->GetNodeID()).x);
            }

            const float ColumnX = ColumnRight - WidestInColumn;

            float CursorY = Origin.y;
            for (CEdGraphNode* Node : Column)
            {
                NodeEditor::SetNodePosition(Node->GetNodeID(), ImVec2(ColumnX, CursorY));
                PlacedY[Node] = CursorY;
                CursorY += NodeEditor::GetNodeSize(Node->GetNodeID()).y + kRowGap;
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
                WidestDead = Math::Max(WidestDead, NodeEditor::GetNodeSize(Node->GetNodeID()).x);
            }

            const float DeadX = ColumnRight - kColumnGap * 2.0f - WidestDead;
            float CursorY = Origin.y;
            for (CEdGraphNode* Node : Dead)
            {
                NodeEditor::SetNodePosition(Node->GetNodeID(), ImVec2(DeadX, CursorY));
                CursorY += NodeEditor::GetNodeSize(Node->GetNodeID()).y + kRowGap;
            }
        }

        NotifyContentChanged();
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
        const bool bEnabled = ax::NodeEditor::GetSelectedObjectCount() >= 2;

        for (const FAlignEntry& Entry : Entries)
        {
            if (Entry.Mode == ENodeAlignment::DistributeX)
            {
                ImGui::Separator();
            }
            if (ImGui::MenuItem(Entry.Label, Entry.Shortcut, false, bEnabled))
            {
                PendingAlignment     = Entry.Mode;
                bHasPendingAlignment = true;
            }
        }
    }

    void CEdNodeGraph::QueueNodePlacement(CEdGraphNode* Node, ImVec2 ScreenPos)
    {
        if (Node != nullptr)
        {
            PendingPlacements.push_back(FPendingPlacement{ Node, ScreenPos });
        }
    }

    uint64 CEdNodeGraph::MakeLinkID(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin)
    {
        return ((uint64)OutputPin->GetPinGUID() << 32) | (uint64)InputPin->GetPinGUID();
    }

    void CEdNodeGraph::DrawGraph()
    {
        LUMINA_PROFILE_SCOPE();
        using namespace ax;
        
        // The node editor hit-tests purely in screen space, so a window drawn over it would also match.
        const bool bHostWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        NodeEditor::SetCurrentEditor(Context);
        NodeEditor::Begin(GetName().c_str());

        // Re-armed here so nobody has to remember to switch them back on after a node's text field.
        NodeEditor::EnableShortcuts(true);

        PushGraphStyle();

        Graph::GraphNodeBuilder NodeBuilder;
        
        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Links;
        Links.reserve(40);

        // A procedurally-built graph carries positions but no saved layout, so seed from them once.
        if (bFirstDraw && GraphSaveData.empty())
        {
            for (CEdGraphNode* Node : Nodes)
            {
                NodeEditor::SetNodePosition(Node->GetNodeID(), ImVec2(Node->GridX, Node->GridY));
            }
        }

        // Applied now that the editor context is current and the screen to canvas transform exists.
        for (const FPendingPlacement& Placement : PendingPlacements)
        {
            if (Placement.Node != nullptr)
            {
                NodeEditor::SetNodePosition(Placement.Node->GetNodeID(), NodeEditor::ScreenToCanvas(Placement.ScreenPos));
            }
        }
        PendingPlacements.clear();

        if (bHasPendingAlignment)
        {
            bHasPendingAlignment = false;
            AlignSelectedNodes(PendingAlignment);
        }

        if (bHasPendingTidy)
        {
            bHasPendingTidy = false;
            TidyGraph();
        }

        // Skipped when the graph declares no root, which keeps it off graphs with no dead-node notion.
        THashSet<CEdGraphNode*> ContributingNodes;
        if (bFadeDeadNodes)
        {
            CollectContributingNodes(ContributingNodes);
        }

        // Collected before submission so a graph drawing its own wires can lay them under the nodes.
        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Links.emplace_back(InputPin, Connection);
                }
            }
        }

        THashMap<uint64, uint32> LinkIndexByID;
        for (uint32 Index = 0; Index < (uint32)Links.size(); ++Index)
        {
            LinkIndexByID[MakeLinkID(Links[Index].first, Links[Index].second)] = Index;
        }

        auto FindLinkIndex = [&LinkIndexByID] (NodeEditor::LinkId LinkID, uint32& OutIndex)
        {
            auto Itr = LinkIndexByID.find((uint64)LinkID.Get());
            if (Itr == LinkIndexByID.end())
            {
                return false;
            }

            OutIndex = Itr->second;
            return true;
        };

        DrawGraphOverlay(Links);

        for (CEdGraphNode* Node : Nodes)
        {
            ImVec2 Position = NodeEditor::GetNodePosition(Node->GetNodeID());
            Node->GridX = Position.x;
            Node->GridY = Position.y;

            // This node reaches no output, so the compiler never emits it, and it costs nothing when connected.
            const bool bDeadNode = !ContributingNodes.empty()
                && ContributingNodes.find(Node) == ContributingNodes.end();
            if (bDeadNode)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
            }

            if (Node->WantsRerouteDotRendering())
            {
                DrawRerouteNode(Node);
                if (bDeadNode)
                {
                    ImGui::PopStyleVar();
                }
                continue;
            }

            if (DrawCustomNode(Node))
            {
                if (bDeadNode)
                {
                    ImGui::PopStyleVar();
                }
                continue;
            }

            NodeBuilder.Begin(Node->GetNodeID());

            if (!Node->WantsTitlebar())
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            }

            const bool bNodeActive = DebugContext.bEnabled && DebugContext.ActiveNodes != nullptr
                && DebugContext.ActiveNodes->find(Node) != DebugContext.ActiveNodes->end();
            const uint32 HeaderColor = bNodeActive ? IM_COL32(235, 170, 40, 255) : Node->GetNodeTitleColor();
            NodeBuilder.Header(ImGui::ColorConvertU32ToFloat4(HeaderColor));

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
                ImGui::BeginVertical("inputs", ImVec2(0,0), 0.0f);
                ImGui::Dummy(ImVec2(0,0));
                ImGui::EndVertical();
            }
            
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                NodeBuilder.Input(InputPin->GetPinGUID());
    
                ImGui::PushID(InputPin);
                {
                    ImVec4 PinColor = ImGui::ColorConvertU32ToFloat4(InputPin->GetPinColor());
                    if (Node->HasError())
                    {
                        PinColor = ImVec4(255.0f, 0.0f, 0.0f, 255.0f);
                    }

                    const bool bDisabled = InputPin->IsDisabled();
                    const float IconAlpha = bDisabled ? 80.0f : 255.0f;
                    if (bDisabled)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
                    }

                    DrawPinIcon(InputPin->HasConnection(), (int)IconAlpha, PinColor);
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
                }
                ImGui::PopID();

                NodeBuilder.EndInput();
            }
    
            NodeBuilder.Middle();
            Node->DrawNodeBody();
            
            for (CEdNodeGraphPin* OutputPin : Node->GetOutputPins())
            {
                NodeBuilder.Output(OutputPin->GetPinGUID());
                
                ImGui::PushID(OutputPin);
                {
                    ImGui::Spring(0);

                    const bool bDisabledOut = OutputPin->IsDisabled();
                    if (bDisabledOut)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
                    }

                    ImGui::Spring(1, 1);
                    DrawPinDebugValue(OutputPin);
                    ImGui::TextUnformatted(OutputPin->GetPinName().c_str());
                    ImGui::Spring(0);

                    ImVec4 PinColor = ImGui::ColorConvertU32ToFloat4(OutputPin->GetPinColor());
                    if (Node->HasError())
                    {
                        PinColor = ImVec4(255.0f, 0.0f, 0.0f, 255.0f);
                    }
                    DrawPinIcon(OutputPin->HasConnection(), bDisabledOut ? 80 : 255, PinColor);

                    if (bDebug)
                    {
                        ImGui::Text("(ID - %i)", OutputPin->GetPinGUID());
                    }

                    if (bDisabledOut)
                    {
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::PopID();
    
                NodeBuilder.EndOutput();
            }
            
            NodeBuilder.End(Node->WantsTitlebar());

            if (bDeadNode)
            {
                ImGui::PopStyleVar();
            }
        }
    
        NodeEditor::Suspend();
        {
            
            // ImGui allows one popup per level, so an unguarded OpenPopup replaces the one just opened.
            if (bHostWindowHovered)
            {
                NodeEditor::NodeId NodeId;
                if (NodeEditor::ShowNodeContextMenu(&NodeId))
                {
                    ContextMenuNodeID = NodeId.Get();
                    ImGui::OpenPopup("Node Context Menu");
                }

                NodeEditor::PinId PinId;
                if (NodeEditor::ShowPinContextMenu(&PinId))
                {
                    ImGui::OpenPopup("Pin Context Menu");
                }

                NodeEditor::LinkId LinkId;
                if (NodeEditor::ShowLinkContextMenu(&LinkId))
                {
                    ImGui::OpenPopup("Link Context Menu");
                }

                if (NodeEditor::ShowBackgroundContextMenu())
                {
                    PendingSourcePin = nullptr;
                    ActionMenu.Reset();
                    ImGui::OpenPopup("Create New Node");
                }
            }

            if (bOpenCreateFromPin)
            {
                bOpenCreateFromPin = false;
                ActionMenu.Reset();
                ImGui::OpenPopup("Create New Node");
            }

            if (ImGui::BeginPopup("Create New Node"))
            {
                DrawGraphContextMenu();
                ImGui::EndPopup();
            }
            else
            {
                PendingSourcePin = nullptr;
            }

            // The popup was opened and never begun, so every node's own context menu was unreachable.
            if (ImGui::BeginPopup("Node Context Menu"))
            {
                auto NodeItr = Algo::FindIf(Nodes.begin(), Nodes.end(), [this](const TObjectPtr<CEdGraphNode>& A)
                {
                    return A.IsValid() && Cmp::Equal(A->GetNodeID(), ContextMenuNodeID);
                });

                DrawNodeContextMenu(NodeItr != Nodes.end() ? NodeItr->Get() : nullptr);
                ImGui::EndPopup();
            }
            
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !NodeEditor::GetHoveredNode()
                && !NodeEditor::GetHoveredPin()
                && !NodeEditor::GetHoveredLink())
            {
                
                // IsKeyDown, since the CLICK is the trigger and the letter acts as a modifier.
                for (int i = ImGuiKey_A; i <= ImGuiKey_Z; ++i)
                {
                    if (ImGui::IsKeyDown((ImGuiKey)i))
                    {
                        HandleQuickPlace((char)('A' + (i - ImGuiKey_A)), NodeEditor::ScreenToCanvas(ImGui::GetMousePos()));
                        break;
                    }
                }
                
                int Digit = -1;
                for (int i = 0; i < 9; ++i)
                {
                    if (ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_1 + i)))
                    {
                        Digit = i + 1;
                        break;
                    }
                }
                if (Digit < 0 && ImGui::IsKeyDown(ImGuiKey_0))
                {
                    Digit = 0;
                }
                if (Digit >= 0)
                {
                    HandleQuickPlace(Digit, NodeEditor::ScreenToCanvas(ImGui::GetMousePos()));
                }
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
                        PendingAlignment     = Hotkey.Mode;
                        bHasPendingAlignment = true;
                        break;
                    }
                }
            }

            HandleRenameShortcut();
        
            if (NodeEditor::BeginShortcut())
            {
                if (NodeEditor::AcceptCopy())
                {
                    TVector<CEdGraphNode*>& CopiedNodes = GetNodeClipboard();
                    CopiedNodes.clear();

                    NodeEditor::NodeId Selections[12];
                    int Num = NodeEditor::GetSelectedNodes(Selections, std::size(Selections));
                
                    ImVec2 Min(FLT_MAX, FLT_MAX);
                    ImVec2 Max(-FLT_MAX, -FLT_MAX);
                
                    for (int i = 0; i < Num; ++i)
                    {
                        NodeEditor::NodeId Selection = Selections[i];
                        auto NodeItr = Algo::FindIf(Nodes.begin(), Nodes.end(), [&] (const TObjectPtr<CEdGraphNode>& A)
                        {
                            return Cmp::Equal(A->GetNodeID(), Selection.Get()) && A->IsDeletable();
                        });
                    
                        if (NodeItr == Nodes.end())
                        {
                            continue;
                        }
                    
                        CopiedNodes.push_back(NodeItr->Get());
                    
                        ImVec2 Pos = NodeEditor::GetNodePosition(Selection);
                        ImVec2 Size = NodeEditor::GetNodeSize(Selection);

                        Min.x = Math::Min(Min.x, Pos.x);
                        Min.y = Math::Min(Min.y, Pos.y);

                        Max.x = Math::Max(Max.x, Pos.x + Size.x);
                        Max.y = Math::Max(Max.y, Pos.y + Size.y);
                    }
                
                    GClipboardPivot = (Min + Max) * 0.5f;
                }
            
                if (NodeEditor::AcceptPaste())
                {
                    NodeEditor::ClearSelection();

                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());

                    // The clipboard is shared, so filter to what THIS graph's registered node classes accept.
                    TVector<CEdGraphNode*> Pastable;
                    Pastable.reserve(GetNodeClipboard().size());
                    uint32 Rejected = 0;
                    for (CEdGraphNode* Copied : GetNodeClipboard())
                    {
                        if (Copied == nullptr)
                        {
                            continue;
                        }

                        const THashSet<CClass*>& Supported = GetSupportedNodes();
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

                    CloneNodes(Pastable, PasteLocation - GClipboardPivot);
                }
            
                if (NodeEditor::AcceptDuplicate())
                {
                    TVector<CEdGraphNode*> DupNodes;
                
                    NodeEditor::NodeId Selections[12];
                    int Num = NodeEditor::GetSelectedNodes(Selections, std::size(Selections));
                
                    ImVec2 Min(FLT_MAX, FLT_MAX);
                    ImVec2 Max(-FLT_MAX, -FLT_MAX);
                
                    for (int i = 0; i < Num; ++i)
                    {
                        NodeEditor::NodeId Selection = Selections[i];
                        auto NodeItr = Algo::FindIf(Nodes.begin(), Nodes.end(), [&] (const TObjectPtr<CEdGraphNode>& A)
                        {
                            return A->GetNodeID() == static_cast<int64>(Selection.Get()) && A->IsDeletable();
                        });
                    
                        if (NodeItr == Nodes.end())
                        {
                            continue;
                        }
                    
                        DupNodes.push_back(NodeItr->Get());
                    
                        ImVec2 Pos = NodeEditor::GetNodePosition(Selection);
                        ImVec2 Size = NodeEditor::GetNodeSize(Selection);

                        Min.x = Math::Min(Min.x, Pos.x);
                        Min.y = Math::Min(Min.y, Pos.y);

                        Max.x = Math::Max(Max.x, Pos.x + Size.x);
                        Max.y = Math::Max(Max.y, Pos.y + Size.y);
                    }
                
                    // A local pivot, since duplicate never leaves this graph and must not disturb the shared one.
                    const ImVec2 DupPivot = (Min + Max) * 0.5f;

                    NodeEditor::ClearSelection();
                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());

                    CloneNodes(DupNodes, PasteLocation - DupPivot);
                }
            }
        }
        NodeEditor::Resume();

        NodeEditor::EndShortcut();
        
        // Firing per node made a multi-selection reassign the table several times a frame.
        CEdGraphNode* SoleSelectedNode = nullptr;
        int32 SelectedNodeCount = 0;

        for (CEdGraphNode* Node : Nodes)
        {
            if (NodeEditor::IsNodeSelected(Node->GetNodeID()))
            {
                ++SelectedNodeCount;
                if (SelectedNodeCount == 1)
                {
                    SoleSelectedNode = Node;
                }
                else
                {
                    // Nothing below needs an exact count, and graphs can be large.
                    break;
                }
            }
        }

        if (NodeSelectedCallback)
        {
            // Reporting none lets consumers fall back rather than picking an arbitrary selection member.
            NodeSelectedCallback(SelectedNodeCount == 1 ? SoleSelectedNode : nullptr);
        }

        // The 1-based IDs match last frame's emission order, so the index is stable.
        if (LinkSelectedCallback)
        {
            NodeEditor::LinkId SelectedLink;
            uint32 LinkIndex = 0;
            if (NodeEditor::GetSelectedLinks(&SelectedLink, 1) == 1)
            {
                if (FindLinkIndex(SelectedLink, LinkIndex))
                {
                    LinkSelectedCallback(Links[LinkIndex].first, Links[LinkIndex].second);
                }
                else
                {
                    LinkSelectedCallback(nullptr, nullptr);
                }
            }
            else
            {
                LinkSelectedCallback(nullptr, nullptr);
            }
        }

        // Double-clicking a node descends into its sub-graph (if it has one).
        if (NodeDoubleClickedCallback)
        {
            if (NodeEditor::NodeId DoubleClickedNode = NodeEditor::GetDoubleClickedNode())
            {
                for (CEdGraphNode* Node : Nodes)
                {
                    if (Cmp::Equal(Node->GetNodeID(), DoubleClickedNode.Get()))
                    {
                        NodeDoubleClickedCallback(Node);
                        break;
                    }
                }
            }
        }

        for (auto& [Start, End] : Links)
        {
            const uint64 ThisLinkID = MakeLinkID(Start, End);

            ImVec4 LinkColor(1.0f, 1.0f, 1.0f, 1.0f);
            float  LinkThickness = 1.0f;
            GetLinkStyle(Start, End, LinkColor, LinkThickness);

            NodeEditor::Link(ThisLinkID, Start->GetPinGUID(), End->GetPinGUID(), LinkColor, LinkThickness);

            // Re-issued each frame to keep the flow animation looping.
            if (DebugContext.bEnabled && DebugContext.bFlowLinks && WantsDebugLinkFlow())
            {
                NodeEditor::Flow(ThisLinkID);
            }
        }

        // MousePos is already canvas space outside Suspend(), so ScreenToCanvas would double-transform.
        if (NodeEditor::LinkId DoubleClickedLink = NodeEditor::GetDoubleClickedLink())
        {
            uint32 LinkIndex = 0;
            if (FindLinkIndex(DoubleClickedLink, LinkIndex) && GetRerouteNodeClass() != nullptr)
            {
                CEdNodeGraphPin* SidePinA = Links[LinkIndex].first;
                CEdNodeGraphPin* SidePinB = Links[LinkIndex].second;

                // Links are (InputPin, OutputPin); normalize to (Output, Input) for InsertRerouteOnLink.
                CEdNodeGraphPin* OutputSide = SidePinA->bInputPin ? SidePinB : SidePinA;
                CEdNodeGraphPin* InputSide  = SidePinA->bInputPin ? SidePinA : SidePinB;

                const ImVec2 InsertAt = ImGui::GetMousePos();
                InsertRerouteOnLink(OutputSide, InputSide, InsertAt);
            }
        }
        
        if (NodeEditor::BeginCreate())
        {
            NodeEditor::PinId StartPinID, EndPinID;
            if (NodeEditor::QueryNewLink(&StartPinID, &EndPinID))
            {
                if (StartPinID && EndPinID && StartPinID != EndPinID)
                {
                    CEdNodeGraphPin* StartPin = nullptr;
                    CEdNodeGraphPin* EndPin = nullptr;

                    const uint32 StartGUID = static_cast<uint32>(StartPinID.Get());
                    const uint32 EndGUID   = static_cast<uint32>(EndPinID.Get());

                    for (CEdGraphNode* Node : Nodes)
                    {
                        if (!StartPin)
                        {
                            StartPin = Node->GetPin(StartGUID, ENodePinDirection::Output);
                            if (!StartPin)
                            {
                                StartPin = Node->GetPin(StartGUID, ENodePinDirection::Input);
                            }
                        }
                        if (!EndPin)
                        {
                            EndPin = Node->GetPin(EndGUID, ENodePinDirection::Output);
                            if (!EndPin)
                            {
                                EndPin = Node->GetPin(EndGUID, ENodePinDirection::Input);
                            }
                        }
                        if (StartPin && EndPin)
                        {
                            break;
                        }
                    }

                    // The user dragged input to output, so swap and keep StartPin on the output side.
                    if (StartPin && EndPin && StartPin->bInputPin && !EndPin->bInputPin)
                    {
                        std::swap(StartPin, EndPin);
                    }

                    const FEdGraphSchema& Schema = GetSchema();
                    const bool bAnyDisabled = (StartPin && StartPin->IsDisabled()) || (EndPin && EndPin->IsDisabled());
                    const bool bValidDirections = StartPin && EndPin && !StartPin->bInputPin && EndPin->bInputPin;

                    if (!bValidDirections || bAnyDisabled || !Schema.CanCreateConnection(StartPin, EndPin))
                    {
                        NodeEditor::RejectNewItem(ImColor(255, 64, 64), 2.0f);
                    }
                    else if (NodeEditor::AcceptNewItem(ImColor(128, 255, 128), 2.0f))
                    {
                        if (EndPin->HasConnection() && !Schema.AllowsMultipleConnections(EndPin))
                        {
                            TVector<CEdNodeGraphPin*> Existing = EndPin->GetConnections();
                            for (CEdNodeGraphPin* ConnectedPin : Existing)
                            {
                                EndPin->DisconnectFrom(ConnectedPin);
                            }
                        }

                        StartPin->AddConnection(EndPin);
                        EndPin->AddConnection(StartPin);
                        NotifyContentChanged();
                        ValidateGraph();
                    }
                }
            }

            // The popup is opened in the Suspend block above on the next iteration.
            NodeEditor::PinId NewNodeFromPinId;
            if (NodeEditor::QueryNewNode(&NewNodeFromPinId))
            {
                if (NodeEditor::AcceptNewItem())
                {
                    CEdNodeGraphPin* SourcePin = nullptr;
                    const uint32 PinGUID = static_cast<uint32>(NewNodeFromPinId.Get());
                    for (CEdGraphNode* Node : Nodes)
                    {
                        SourcePin = Node->GetPin(PinGUID, ENodePinDirection::Output);
                        if (SourcePin) break;
                        SourcePin = Node->GetPin(PinGUID, ENodePinDirection::Input);
                        if (SourcePin) break;
                    }

                    if (SourcePin && !SourcePin->IsDisabled())
                    {
                        PendingSourcePin = SourcePin;
                        bOpenCreateFromPin = true;
                    }
                }
            }
        }

        NodeEditor::EndCreate();
        
        if (NodeEditor::BeginDelete())
        {
            NodeEditor::NodeId NodeId = 0;
            while (NodeEditor::QueryDeletedNode(&NodeId))
            {
                // O(n^2) scan mirrors the approach from the imgui-node-editor examples; acceptable for typical graph sizes.
                auto NodeItr = Algo::FindIf(Nodes.begin(), Nodes.end(), [NodeId] (const TObjectPtr<CEdGraphNode>& A)
                {
                    return Cmp::Equal(A->GetNodeID(), NodeId.Get()) && A->IsDeletable();
                });

                if (NodeItr != Nodes.end())
                {
                    CEdGraphNode* Node = *NodeItr;
                    if (!NodeEditor::AcceptDeletedItem())
                    {
                        continue;
                    }

                    if (PreNodeDeletedCallback)
                    {
                        PreNodeDeletedCallback(Node);
                    }
                    
                    for (CEdNodeGraphPin* Pin : Node->GetInputPins())
                    {
                        if (Pin->HasConnection())
                        {
                            TVector<CEdNodeGraphPin*> PinConnections = Pin->GetConnections();
                            for (CEdNodeGraphPin* ConnectedPin : PinConnections)
                            {
                                ConnectedPin->DisconnectFrom(Pin);
                            }
                            Pin->ClearConnections();
                        }
                    }
        
                    for (CEdNodeGraphPin* Pin : Node->GetOutputPins())
                    {
                        if (Pin->HasConnection())
                        {
                            TVector<CEdNodeGraphPin*> PinConnections = Pin->GetConnections();
                            for (CEdNodeGraphPin* ConnectedPin : PinConnections)
                            {
                                ConnectedPin->DisconnectFrom(Pin);
                            }
                            Pin->ClearConnections();
                        }
                    }
                    
                    Nodes.erase(NodeItr);

                    // The copy buffer holds raw pointers, so a node deleted between copy and paste would be cloned dead.
                    ForgetClipboardNode(Node);

                    Node->ConditionalBeginDestroy();
                    Node = nullptr;

                    NotifyContentChanged();
                    ValidateGraph();
                }
            }
            
            
            NodeEditor::LinkId DeletedLinkId;
            while (NodeEditor::QueryDeletedLink(&DeletedLinkId))
            {
                if (NodeEditor::AcceptDeletedItem())
                {
                    uint32 LinkIndex = 0;
                    if (FindLinkIndex(DeletedLinkId, LinkIndex))
                    {
                        const TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>& Pair = Links[LinkIndex];
                        Pair.first->RemoveConnection(Pair.second);
                        Pair.second->RemoveConnection(Pair.first);
                        NotifyContentChanged();
                        ValidateGraph();
                    }
                }
            }
        }
        
        NodeEditor::EndDelete();

        PopGraphStyle();

        NodeEditor::End();
        NodeEditor::SetCurrentEditor(nullptr);

        // The host window is current again, and a spawned node lands in PendingPlacements for next frame.
        DrawCanvasDropTarget();

        bFirstDraw = false;
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
            QueueTidyGraph();
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

    CEdGraphNode* CEdNodeGraph::CreateNode(CClass* NodeClass)
    {
        CEdGraphNode* NewNode = NewObject<CEdGraphNode>(NodeClass, GetNodeOuter());
        AddNode(NewNode);
        return NewNode;
    }

    void CEdNodeGraph::HandleRenameShortcut()
    {
        using namespace ax;

        const bool bCanTakeKey = !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (bCanTakeKey && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            // A second selected node makes the target ambiguous, so rename nothing.
            NodeEditor::NodeId Selection[2];
            if (NodeEditor::GetSelectedNodes(Selection, 2) == 1)
            {
                CEdGraphNode* Node = FindNode((int64)Selection[0].Get());

                FString Current;
                if (Node != nullptr && Node->GetRenameText(Current))
                {
                    RenameNodeID = Node->GetNodeID();
                    const size_t Count = Math::Min(Current.size(), sizeof(RenameBuffer) - 1);
                    Memory::Memcpy(RenameBuffer, Current.data(), Count);
                    RenameBuffer[Count] = '\0';
                    bOpenRenamePopup = true;
                }
            }
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

    CEdGraphNode* CEdNodeGraph::FindNode(int64 InNodeID) const
    {
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
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
        const TVector<TObjectPtr<CEdNodeGraphPin>>& Candidates = bSourceIsInput
            ? NewNode->GetOutputPins()
            : NewNode->GetInputPins();

        for (const TObjectPtr<CEdNodeGraphPin>& Candidate : Candidates)
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
        CEdNodeGraphPin* From = bSourceIsInput ? TargetPin : SourcePin;
        CEdNodeGraphPin* To   = bSourceIsInput ? SourcePin : TargetPin;

        const FEdGraphSchema& Schema = GetSchema();
        if (From->IsDisabled() || To->IsDisabled() || !Schema.CanCreateConnection(From, To))
        {
            return;
        }

        if (To->HasConnection() && !Schema.AllowsMultipleConnections(To))
        {
            TVector<CEdNodeGraphPin*> Existing = To->GetConnections();
            for (CEdNodeGraphPin* ConnectedPin : Existing)
            {
                To->DisconnectFrom(ConnectedPin);
            }
        }

        From->AddConnection(To);
        To->AddConnection(From);
        NotifyContentChanged();
        ValidateGraph();
    }

    int64 CEdNodeGraph::GenerateUniqueNodeID(int64 PreferredID) const
    {
        const auto IsTaken = [this](int64 Candidate)
        {
            for (const TObjectPtr<CEdGraphNode>& Existing : Nodes)
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
    
}
