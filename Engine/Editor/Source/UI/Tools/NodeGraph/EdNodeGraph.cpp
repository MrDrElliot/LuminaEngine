#include "EdNodeGraph.h"

#include "EdGraphNode.h"
#include "EdNode_Reroute.h"
#include "GraphAlgorithms.h"
#include "Core/Object/Class.h"
#include <Core/Reflection/Type/LuminaTypes.h>
#include "Drawing.h"
#include "imgui_internal.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "EASTL/sort.h"
#include "imgui-node-editor/imgui_node_editor_internal.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // ONE clipboard for every graph in the process, not one per graph instance. Copying in one
        // material editor and pasting into another is the point; a per-graph buffer made Ctrl+C/Ctrl+V
        // work only within the graph you copied from.
        //
        // Raw pointers, deliberately: a TObjectPtr clipboard would keep nodes alive past their graph and
        // hand the destruction-order problem to static teardown. Lifetime is handled by scrubbing
        // instead -- on node delete, and on graph shutdown for everything that graph owns.
        TVector<CEdGraphNode*>& GetNodeClipboard()
        {
            static TVector<CEdGraphNode*> Clipboard;
            return Clipboard;
        }

        // Canvas-space centre of the copied set, so a paste lands relative to the cursor.
        ImVec2 GClipboardPivot(0.0f, 0.0f);

        void ForgetClipboardNode(CEdGraphNode* Node)
        {
            TVector<CEdGraphNode*>& Clipboard = GetNodeClipboard();
            Clipboard.erase(eastl::remove(Clipboard.begin(), Clipboard.end(), Node), Clipboard.end());
        }
    }

    // Display names may carry spaces or punctuation ("Curve Sample", "Two-Bone IK"), but FullName becomes
    // the emitted shader variable, so anything outside [A-Za-z0-9_] has to collapse to an underscore.
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
        // This graph's nodes die with it, so drop anything of ours still on the shared clipboard --
        // otherwise closing the editor you copied from leaves a paste pointing at freed nodes.
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

        // Rebuild Connections from restored pin links: AddNode above ran ValidateGraph() before any links
        // existed, so without this a save after load (no edit to re-trigger it) drops every link.
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
        ImGui::PushID(Node->GetNodeID());

        const ImVec2 CursorStart = ImGui::GetCursorScreenPos();
        const ImVec2 DotSize(DotRadius * 2.0f, DotRadius * 2.0f);

        // Reserve a square for the dot. The hit area extends past it on each side so dragging a
        // wire onto the dot consistently hits one of the pins instead of the empty node body.
        ImGui::Dummy(DotSize);

        const ImVec2 Center      = CursorStart + DotSize * 0.5f;
        const float  HitHalfX    = DotRadius * 2.0f;
        const ImVec2 InputRectMin (CursorStart.x - HitHalfX, CursorStart.y - DotRadius * 0.5f);
        const ImVec2 InputRectMax (Center.x,                  CursorStart.y + DotSize.y + DotRadius * 0.5f);
        const ImVec2 OutputRectMin(Center.x,                  InputRectMin.y);
        const ImVec2 OutputRectMax(CursorStart.x + DotSize.x + HitHalfX, InputRectMax.y);

        // Input pin: hit-tests against the LEFT half of the (enlarged) dot. Both pins share the
        // same pivot point at the centre so wires visually meet at a single dot.
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

        // Output pin: RIGHT half of the dot's hit area. Same pivot as the input so the visual dot
        // remains a single point.
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

        // Paint the dot itself on the node's background draw list so it sits behind incoming wires
        // but above the (transparent) node background.
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

    // Pins are matched by name rather than by index: a node whose pins are built from its properties
    // (the material function-call node) can rebuild them lazily after the clone is constructed, and an
    // index match would then silently attach the wrong pin. Missing a link is recoverable; mis-wiring
    // one is not.
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
            Clones.emplace(Source, Clone);

            NodeEditor::SetNodePosition(Clone->GetNodeID(), NodeEditor::GetNodePosition(Source->GetNodeID()) + Delta);
            NodeEditor::SelectNode(Clone->GetNodeID(), true);
        }

        // Second pass, once every clone exists: a link can point forwards in the list as easily as
        // backwards. Walking INPUT pins only visits each link exactly once (same traversal the draw
        // loop uses to collect them), so nothing gets connected twice.
        for (CEdGraphNode* Source : SourceNodes)
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

                    // Only when the far end was copied too. A link to a node outside the set would
                    // otherwise hang the clone off the original's neighbour, quietly editing a part of
                    // the graph the user never selected.
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

        ValidateGraph();
    }

    void CEdNodeGraph::AlignSelectedNodes(ENodeAlignment Alignment)
    {
        using namespace ax;

        // GetSelectedObjectCount counts links too, so it is an upper bound rather than the node count;
        // GetSelectedNodes reports how many it actually wrote.
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

        // Distribute walks the selection in screen order rather than selection order, otherwise the
        // nodes get shuffled into whatever order they happened to be clicked in.
        const bool bDistributeX = Alignment == ENodeAlignment::DistributeX;
        const bool bDistributeY = Alignment == ENodeAlignment::DistributeY;
        if (bDistributeX || bDistributeY)
        {
            if (Count < 3)
            {
                return;   // two nodes are already evenly spaced; nothing to solve
            }

            eastl::sort(Entries.begin(), Entries.end(), [bDistributeX](const FEntry& A, const FEntry& B)
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

        // No roots means no way to tell live from dead. Reporting every node as dead would be worse than
        // reporting none, so say nothing.
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

        // Routed through the editor's own delete request rather than erasing here: that path already
        // unhooks every pin, forgets the clipboard entry and re-validates. Duplicating it is how the two
        // drift apart.
        for (CEdGraphNode* Node : Dead)
        {
            NodeEditor::DeleteNode(Node->GetNodeID());
        }
    }

    void CEdNodeGraph::TidyGraph()
    {
        using namespace ax;

        // Column spacing is generous because material wires read left-to-right and cramped columns make
        // long wires ambiguous; row spacing is tight since nodes in a column rarely relate to each other.
        constexpr float kColumnGap = 90.0f;
        constexpr float kRowGap    = 28.0f;

        THashSet<CEdGraphNode*> Contributing;
        CollectContributingNodes(Contributing);
        if (Contributing.empty())
        {
            return;
        }

        // Depth = longest path from this node to a root, so a node sits one column left of its FURTHEST
        // consumer. Taking the longest (not the shortest) is what stops a wire skipping backwards over a
        // column, which is the thing that makes an auto-layout look wrong.
        THashMap<CEdGraphNode*, int32> Depth;
        TVector<CEdGraphNode*> Sorted;
        GraphAlgorithms::TopologicalSortReachable(Nodes, Contributing, Sorted);

        // Sorted is dependency order (producers first), so walking it backwards visits every consumer
        // before its producers -- exactly the order the relaxation below needs.
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

        // Order within a column by the mean Y of each node's consumers, a single barycentric pass. It is
        // not optimal crossing reduction, but it is stable, O(edges), and removes the obvious tangles.
        TVector<TVector<CEdGraphNode*>> Columns;
        Columns.resize(MaxDepth + 1);
        for (const auto& Pair : Depth)
        {
            Columns[Pair.second].push_back(Pair.first);
        }

        const ImVec2 Origin = NodeEditor::GetNodePosition(Sorted.empty() ? Nodes[0]->GetNodeID() : Sorted.back()->GetNodeID());

        THashMap<CEdGraphNode*, float> PlacedY;

        // Roots (depth 0) are the rightmost column; walk outward so a column is ordered against consumers
        // that already have their final Y.
        float ColumnRight = Origin.x;
        for (int32 D = 0; D <= MaxDepth; ++D)
        {
            TVector<CEdGraphNode*>& Column = Columns[D];

            eastl::stable_sort(Column.begin(), Column.end(), [&](CEdGraphNode* A, CEdGraphNode* B)
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

        // Dead nodes are parked in their own column to the left of everything, so tidying never silently
        // buries work-in-progress inside the live graph.
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

        // Alignment needs two nodes to mean anything; grey the whole set out rather than offering
        // items that silently do nothing.
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

    void CEdNodeGraph::DrawGraph()
    {
        LUMINA_PROFILE_SCOPE();
        using namespace ax;
        
        // Sampled BEFORE NodeEditor::Begin, while the graph's host window is still the current one. The
        // node editor hit-tests its canvas purely in screen space and has no idea another ImGui window may
        // be drawn over it, so without this a right-click on a docked window sitting ON TOP of the graph
        // also reads as a canvas right-click.
        const bool bHostWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        NodeEditor::SetCurrentEditor(Context);
        NodeEditor::Begin(GetName().c_str());

        // Shortcuts are re-armed every frame. A node drawing a text field turns them off for that frame
        // so Ctrl+C/V/X reach the field instead of copy/pasting nodes (ShortcutAction::Accept gates on
        // nothing but focus and this flag). Re-arming here means no one has to remember to switch them
        // back on -- including when the node being edited is deleted mid-edit.
        NodeEditor::EnableShortcuts(true);

        PushGraphStyle();

        Graph::GraphNodeBuilder NodeBuilder;
        
        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Links;
        Links.reserve(40);

        // Procedurally-built graphs (e.g. import-generated materials) carry node positions in GridX/GridY but
        // have no saved editor layout; seed the editor from them on first draw so the graph isn't stacked at
        // the origin. Interactive graphs always have GraphSaveData after their first save, so this is skipped.
        if (bFirstDraw && GraphSaveData.empty())
        {
            for (CEdGraphNode* Node : Nodes)
            {
                NodeEditor::SetNodePosition(Node->GetNodeID(), ImVec2(Node->GridX, Node->GridY));
            }
        }

        // Apply placements queued from outside the draw loop (drop handlers), now that the editor
        // context is current and the screen->canvas transform is available.
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

        // One analysis pass per draw, reused by the fade below. Skipped entirely when the graph declares
        // no root, which is also what keeps this off graphs that have no notion of a dead node.
        THashSet<CEdGraphNode*> ContributingNodes;
        if (bFadeDeadNodes)
        {
            CollectContributingNodes(ContributingNodes);
        }

        // Collected before anything is submitted so a graph drawing its own wires (the state machine
        // canvas) can lay them out under the nodes. Order matches the node/pin/connection walk the
        // rest of the pass assumes when mapping a link id back to its pins.
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

        DrawGraphOverlay(Links);

        uint32 Index = 0;
        for (CEdGraphNode* Node : Nodes)
        {
            ImVec2 Position = NodeEditor::GetNodePosition(Node->GetNodeID());
            Node->GridX = Position.x;
            Node->GridY = Position.y;

            // Faded: this node's result reaches no output, so the compiler never emits it. Cheaper to
            // read at a glance than any badge, and it costs nothing when the graph is fully connected.
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
                ++Index;
                continue;
            }

            if (DrawCustomNode(Node))
            {
                if (bDeadNode)
                {
                    ImGui::PopStyleVar();
                }
                ++Index;
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
                ImGui::Text("(ID - %lld)", Node->GetNodeID());
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

                    DrawPinIcon(InputPin->HasConnection(), IconAlpha, PinColor);
                    ImGui::Spring(0);

                    ImGui::TextUnformatted(InputPin->GetPinName().c_str());

                    // Inline editor for unconnected pins; spring aligns editors in a column.
                    // Opt-in per graph via ShouldDrawInlinePinEditors().
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
                    DrawPinIcon(OutputPin->HasConnection(), bDisabledOut ? 80.0f : 255.0f, PinColor);

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

            Index++;
        }
    
        NodeEditor::Suspend();
        {
            
            // Only claim a right-click that actually landed on this graph. ImGui allows one popup at a
            // level, so an unguarded OpenPopup here REPLACES the one the window on top just opened --
            // which is why right-clicking a content-browser tile docked over a material graph opened
            // nothing at all.
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

            // The popup above was being opened and never begun, so right-clicking a node showed nothing
            // and every node's own DrawContextMenu (Make Parameter, Make Texture Parameter, ...) was
            // unreachable.
            if (ImGui::BeginPopup("Node Context Menu"))
            {
                auto NodeItr = eastl::find_if(Nodes.begin(), Nodes.end(), [this](const TObjectPtr<CEdGraphNode>& A)
                {
                    return A.IsValid() && std::cmp_equal(A->GetNodeID(), ContextMenuNodeID);
                });

                DrawNodeContextMenu(NodeItr != Nodes.end() ? NodeItr->Get() : nullptr);
                ImGui::EndPopup();
            }
            
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !NodeEditor::GetHoveredNode()
                && !NodeEditor::GetHoveredPin()
                && !NodeEditor::GetHoveredLink())
            {
                
                // Letter quick-place, same gesture as the digit row below: hold the key, click empty
                // canvas. IsKeyDown rather than IsKeyPressed because the CLICK is the trigger and the
                // letter is acting as a modifier. ImGuiKey_A is an enum value, not the character 'A',
                // so the index has to be mapped back -- passing the raw enum through was why this
                // never matched anything when it was first written.
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

            // Alignment hotkeys. Shift+letter rather than Shift+arrow: keyboard nav is enabled app-wide,
            // so ImGui eats the arrows. Applied next frame through PendingAlignment like the menu items.
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
                        auto NodeItr = eastl::find_if(Nodes.begin(), Nodes.end(), [&] (const TObjectPtr<CEdGraphNode>& A)
                        {
                            return std::cmp_equal(A->GetNodeID(), Selection.Get()) && A->IsDeletable();
                        });
                    
                        if (NodeItr == Nodes.end())
                        {
                            continue;
                        }
                    
                        CopiedNodes.push_back(NodeItr->Get());
                    
                        ImVec2 Pos = NodeEditor::GetNodePosition(Selection);
                        ImVec2 Size = NodeEditor::GetNodeSize(Selection);

                        Min.x = eastl::min(Min.x, Pos.x);
                        Min.y = eastl::min(Min.y, Pos.y);

                        Max.x = eastl::max(Max.x, Pos.x + Size.x);
                        Max.y = eastl::max(Max.y, Pos.y + Size.y);
                    }
                
                    GClipboardPivot = (Min + Max) * 0.5f;
                }
            
                if (NodeEditor::AcceptPaste())
                {
                    NodeEditor::ClearSelection();

                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());

                    // The clipboard is shared across every graph in the editor, so filter to what THIS
                    // graph accepts: pasting an animation node into a material graph would build a node
                    // its compiler cannot walk. Registered node classes are the graph's own definition
                    // of what it can hold, so that is the test.
                    TVector<CEdGraphNode*> Pastable;
                    Pastable.reserve(GetNodeClipboard().size());
                    uint32 Rejected = 0;
                    for (CEdGraphNode* Copied : GetNodeClipboard())
                    {
                        if (Copied == nullptr)
                        {
                            continue;
                        }

                        if (SupportedNodes.find(Copied->GetClass()) != SupportedNodes.end())
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
                        auto NodeItr = eastl::find_if(Nodes.begin(), Nodes.end(), [&] (const TObjectPtr<CEdGraphNode>& A)
                        {
                            return A->GetNodeID() == Selection.Get() && A->IsDeletable();
                        });
                    
                        if (NodeItr == Nodes.end())
                        {
                            continue;
                        }
                    
                        DupNodes.push_back(NodeItr->Get());
                    
                        ImVec2 Pos = NodeEditor::GetNodePosition(Selection);
                        ImVec2 Size = NodeEditor::GetNodeSize(Selection);

                        Min.x = eastl::min(Min.x, Pos.x);
                        Min.y = eastl::min(Min.y, Pos.y);

                        Max.x = eastl::max(Max.x, Pos.x + Size.x);
                        Max.y = eastl::max(Max.y, Pos.y + Size.y);
                    }
                
                    // Local pivot: duplicate never leaves this graph, so it must not disturb the shared
                    // copy/paste clipboard's pivot.
                    const ImVec2 DupPivot = (Min + Max) * 0.5f;

                    NodeEditor::ClearSelection();
                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());

                    CloneNodes(DupNodes, PasteLocation - DupPivot);
                }
            }
        }
        NodeEditor::Resume();

        NodeEditor::EndShortcut();
        
        // Report the selection once per frame, not once per selected node. Firing per node made every
        // consumer bind its property table to each selected node in turn, so a multi-selection
        // reassigned the table several times a frame and it never settled on anything editable.
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
            // A multi-selection has no single object to edit, and the property table holds one object.
            // Reporting none lets consumers fall back to their default view rather than picking an
            // arbitrary member of the selection.
            NodeSelectedCallback(SelectedNodeCount == 1 ? SoleSelectedNode : nullptr);
        }

        // Surface the single selected link to the graph. Their 1-based IDs match last frame's
        // emission order, so the index is stable.
        if (LinkSelectedCallback)
        {
            NodeEditor::LinkId SelectedLink;
            if (NodeEditor::GetSelectedLinks(&SelectedLink, 1) == 1)
            {
                const uint64 LinkIndex = SelectedLink.Get() - 1u;
                if (LinkIndex < Links.size())
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
                    if (std::cmp_equal(Node->GetNodeID(), DoubleClickedNode.Get()))
                    {
                        NodeDoubleClickedCallback(Node);
                        break;
                    }
                }
            }
        }

        uint32 LinkID = 1;
        for (auto& [Start, End] : Links)
        {
            const uint32 ThisLinkID = LinkID++;

            ImVec4 LinkColor(1.0f, 1.0f, 1.0f, 1.0f);
            float  LinkThickness = 1.0f;
            GetLinkStyle(Start, End, LinkColor, LinkThickness);

            NodeEditor::Link(ThisLinkID, Start->GetPinGUID(), End->GetPinGUID(), LinkColor, LinkThickness);

            // Debug: animate flow along every wire so the running graph reads as
            // "live". Re-issued each frame to keep the animation looping.
            if (DebugContext.bEnabled && DebugContext.bFlowLinks)
            {
                NodeEditor::Flow(ThisLinkID);
            }
        }

        // Double-click a wire to insert a reroute at the click position.
        // MousePos is already in canvas space outside Suspend(); ScreenToCanvas would double-transform.
        if (NodeEditor::LinkId DoubleClickedLink = NodeEditor::GetDoubleClickedLink())
        {
            const uint64 LinkIndex = DoubleClickedLink.Get() - 1u;
            if (LinkIndex < Links.size() && GetRerouteNodeClass() != nullptr)
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

                    // User dragged input -> output: swap so StartPin is always the output side.
                    if (StartPin && EndPin && StartPin->bInputPin && !EndPin->bInputPin)
                    {
                        eastl::swap(StartPin, EndPin);
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

            // Drag-from-pin released on empty space: capture the source pin and queue the
            // create-node popup. The popup is opened in the Suspend block above on the next iteration.
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
                auto NodeItr = eastl::find_if(Nodes.begin(), Nodes.end(), [NodeId] (const TObjectPtr<CEdGraphNode>& A)
                {
                    return std::cmp_equal(A->GetNodeID(), NodeId.Get()) && A->IsDeletable();
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

                    // The copy buffer holds raw pointers, so a node deleted between Ctrl+C and Ctrl+V
                    // would be cloned out of freed memory.
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
                    uint64 LinkIndex = DeletedLinkId.Get() - 1u;
                    if (LinkIndex < Links.size())
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

        // After End(): the host window is current again, and any node the hook spawns lands in
        // PendingPlacements for the next frame's screen->canvas conversion.
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

        // Disabled rather than hidden when the graph is clean: a menu whose entries come and go is harder
        // to learn than one that tells you there is nothing to do.
        ImGui::BeginDisabled(Dead.empty());
        if (ImGui::MenuItem(Dead.empty()
                ? LE_ICON_SELECTION_OFF " No Unused Nodes"
                : FFixedString(FFixedString::CtorSprintf(), LE_ICON_SELECTION " Select %d Unused Node%s",
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

        // Never hand out 0: it is the "unassigned" sentinel AddNode tests against, so a node holding it
        // would be re-rolled on any later pass through here.
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
        // Uniqueness is not cosmetic: the node editor keys nodes on this ID and callers scope each node's
        // ImGui widget IDs by it, so a duplicate silently merges two nodes' interaction state rather than
        // failing outright.
        const int64 NodeID = GenerateUniqueNodeID(InNode->NodeID);

        if (InNode->NodeID != 0 && NodeID != InNode->NodeID)
        {
            LOG_WARN("Node graph: '{}' was added with an already-used node ID {}; reassigned to {}. "
                     "Saved links referencing the old ID will not resolve.",
                     InNode->GetNodeDisplayName(), InNode->NodeID, NodeID);
        }

        InNode->PinHashName = FString(InNode->GetNodeDisplayName()) + "_" + eastl::to_string(NodeID);
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
        ValidateGraph();

        return NodeID;
    }
    
}
