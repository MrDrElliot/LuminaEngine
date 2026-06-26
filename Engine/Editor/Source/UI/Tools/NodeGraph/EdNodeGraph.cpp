#include "EdNodeGraph.h"

#include "EdGraphNode.h"
#include "EdNode_Reroute.h"
#include "Core/Object/Class.h"
#include <Core/Reflection/Type/LuminaTypes.h>
#include "Drawing.h"
#include "imgui_internal.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "EASTL/sort.h"
#include "imgui-node-editor/imgui_node_editor_internal.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
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

    void CEdNodeGraph::DrawRerouteNode(CEdGraphNode* Node, TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& OutLinks)
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
            for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
            {
                OutLinks.emplace_back(InputPin, Connection);
            }

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

    void CEdNodeGraph::DrawGraph()
    {
        LUMINA_PROFILE_SCOPE();
        using namespace ax;
        
        NodeEditor::SetCurrentEditor(Context);
        NodeEditor::Begin(GetName().c_str());
    
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

        uint32 Index = 0;
        for (CEdGraphNode* Node : Nodes)
        {
            ImVec2 Position = NodeEditor::GetNodePosition(Node->GetNodeID());
            Node->GridX = Position.x;
            Node->GridY = Position.y;

            if (Node->IsRerouteNode())
            {
                DrawRerouteNode(Node, Links);
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
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Links.emplace_back(InputPin, Connection);
                }
                
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
            Index++;
        }
    
        NodeEditor::Suspend();
        {
            
            NodeEditor::NodeId NodeId;
            if (NodeEditor::ShowNodeContextMenu(&NodeId))
            {
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
            
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !NodeEditor::GetHoveredNode()
                && !NodeEditor::GetHoveredPin()
                && !NodeEditor::GetHoveredLink())
            {
                
                for (int i = ImGuiKey_A; i < ImGuiKey_Z; ++i)
                {
                    if (ImGui::IsKeyDown((ImGuiKey)i))
                    {
                        //HandleQuickPlace((char)i, NodeEditor::ScreenToCanvas(ImGui::GetMousePos()));
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
        
            if (NodeEditor::BeginShortcut())
            {
                static ImVec2 CopiedPivot;
                if (NodeEditor::AcceptCopy())
                {
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
                            return A->GetNodeID() == Selection.Get() && A->IsDeletable();
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
                
                    CopiedPivot = (Min + Max) * 0.5f;
                }
            
                if (NodeEditor::AcceptPaste())
                {
                    NodeEditor::ClearSelection();
                
                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());

                    ImVec2 Delta = PasteLocation - CopiedPivot;
                
                    for (CEdGraphNode* Node : CopiedNodes)
                    {
                        ImVec2 CopiedCanvasPosition = NodeEditor::GetNodePosition(Node->GetNodeID());
                    
                        CEdGraphNode* NewNode = CreateNode(Node->GetClass());
                        Node->CopyPropertiesTo(NewNode);
                    
                        ImVec2 NewPosition = CopiedCanvasPosition + Delta;
                        NodeEditor::SetNodePosition(NewNode->GetNodeID(), NewPosition);
                        NodeEditor::SelectNode(NewNode->GetNodeID(), true);
                    }
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
                
                    CopiedPivot = (Min + Max) * 0.5f;
                
                    NodeEditor::ClearSelection();
                    ImVec2 PasteLocation = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());
                    ImVec2 Delta = PasteLocation - CopiedPivot;
                
                    for (CEdGraphNode* Node : DupNodes)
                    {
                        ImVec2 CopiedCanvasPosition = NodeEditor::GetNodePosition(Node->GetNodeID());
                    
                        CEdGraphNode* NewNode = CreateNode(Node->GetClass());
                        Node->CopyPropertiesTo(NewNode);

                        ImVec2 NewPosition = CopiedCanvasPosition + Delta;
                        NodeEditor::SetNodePosition(NewNode->GetNodeID(), NewPosition);
                        NodeEditor::SelectNode(NewNode->GetNodeID(), true);
                    }
                }
            }
        }
        NodeEditor::Resume();

        NodeEditor::EndShortcut();
        
        bool bAnyNodeSelected = false;
        for (CEdGraphNode* Node : Nodes)
        {
            if (NodeEditor::IsNodeSelected(Node->GetNodeID()))
            {
                bAnyNodeSelected = true;
                if (NodeSelectedCallback)
                {
                    NodeSelectedCallback(Node);
                }
            }
        }
    
        if (!bAnyNodeSelected && NodeSelectedCallback)
        {
            NodeSelectedCallback(nullptr);
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
            NodeEditor::Link(ThisLinkID, Start->GetPinGUID(), End->GetPinGUID());

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

                    Node->ConditionalBeginDestroy();
                    Node = nullptr;
                    
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
                        ValidateGraph();
                    }
                }
            }
        }
        
        NodeEditor::EndDelete();
        
        NodeEditor::End();
        NodeEditor::SetCurrentEditor(nullptr);

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
        constexpr ImVec2 PopupSize(320, 450);
        constexpr float CategorySpacing = 4.0f;

        ImGui::SetNextWindowSize(PopupSize, ImGuiCond_Always);
        
        
        constexpr float HeaderHeight = 54;
        constexpr float FooterHeight = 32;
        ImVec2 ChildSize = ImVec2(PopupSize.x, PopupSize.y - HeaderHeight - FooterHeight);
        
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, CategorySpacing));
        
        if (ImGui::BeginChild("##NodeList", ChildSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.08f, 0.08f, 0.10f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.35f, 0.35f, 0.37f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.45f, 0.45f, 0.47f, 1.0f));
            
            if (ImGui::BeginChild("##ScrollRegion", ImVec2(0, 0), false))
            {
                ImGuiX::Text("Node: {}", Node->GetNodeDisplayName());
                
                ImGui::EndChild();
            }
            
            ImGui::PopStyleColor(4);
            ImGui::EndChild();
        }
        
        ImGui::PopStyleVar(2);
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
        ValidateGraph();
    }

    uint64 CEdNodeGraph::AddNode(CEdGraphNode* InNode)
    {
        int64 NodeID;
        
        if (InNode->NodeID != 0)
        {
            NodeID = InNode->GetNodeID();
        }
        else
        {
            NodeID = Math::RandRange(0u, UINT32_MAX);
        }
        
        InNode->FullName = InNode->GetNodeDisplayName() + "_" + eastl::to_string(NodeID);
        InNode->NodeID = NodeID;
        InNode->OwningGraph = this;

        Nodes.push_back(InNode);

        if (!InNode->bWasBuild)
        {
            InNode->BuildNode();
            InNode->bWasBuild = true;
        }
        
        ValidateGraph();

        return NodeID;
    }
    
}
