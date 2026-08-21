#include "EdGraphNode.h"

#include "EdNodeGraph.h"
#include "EdNodeGraphPin.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectAllocator.h"
#include "Material/MaterialGraphTypes.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Containers/StringFormat.h"

namespace Lumina
{

    static uint32 HashPinID(const FString& NodeName, const FString& PinName, ENodePinDirection Direction)
    {
        FString Composite = NodeName + "_" + PinName + "_" + Format("{}", (uint8)Direction);
        return Hash::GetHash32(Composite);
    }
    
    void CEdGraphNode::PostCreateCDO()
    {
        CObject::PostCreateCDO();
    }

    bool CEdGraphNode::IsSupportedInGraph(CClass* GraphClass) const
    {
        CClass* SupportedGraph = GetSupportedGraphClass();

        // IsChildOf is reflexive, which is what puts every material node in a material function graph.
        return SupportedGraph != nullptr && GraphClass != nullptr && GraphClass->IsChildOf(SupportedGraph);
    }

    ImVec2 CEdGraphNode::GetMinNodeTitleBarSize() const
    {
        const FString Title = GetNodeTitleText();
        return ImVec2(ImGui::CalcTextSize(Title.c_str(), Title.c_str() + Title.size()).x, 28);
    }

    void CEdGraphNode::PushNodeStyle()
    {
        using namespace ax;

        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg,        ImColor(128, 128, 128, 200));
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder,    ImColor( 32,  32,  32, 200));
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_PinRect,       ImColor( 60, 180, 255, 150));
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_PinRectBorder, ImColor( 60, 180, 255, 150));

        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodePadding,  ImVec4(0, 0, 0, 0));
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeRounding, 10);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_SourceDirection, ImVec2(0.0f,  1.0f));
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_TargetDirection, ImVec2(0.0f, -1.0f));
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_LinkStrength, 0.0f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_PinBorderWidth, 1.0f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_PinRadius, 5.0f);
    }

    void CEdGraphNode::PopNodeStyle()
    {
        using namespace ax;

        NodeEditor::PopStyleColor(4);
        NodeEditor::PopStyleVar(7);
    }

    void CEdGraphNode::NotifyValueEdited()
    {
        if (OwningGraph != nullptr)
        {
            OwningGraph->NotifyContentChanged();
        }
    }

    FString CEdGraphNode::GetNodeTitleText() const
    {
        return FString(GetNodeDisplayName());
    }

    void CEdGraphNode::DrawNodeTitleBar()
    {
        const FString Title = GetNodeTitleText();
        if (HasError())
        {
            ImGui::TextColored(ImVec4(255.0f, 0.0f, 0.0f, 255.f), LE_ICON_EXCLAMATION_THICK " %s", Title.c_str());
        }
        else
        {
            ImGui::TextUnformatted(Title.c_str());
        }
    }

    CEdNodeGraphPin* CEdGraphNode::GetRerouteSourcePin() const
    {
        const TVector<TObjectPtr<CEdNodeGraphPin>>& Inputs = GetInputPins();
        return Inputs.empty() ? nullptr : Inputs[0].Get();
    }

    CEdNodeGraphPin* CEdGraphNode::GetPin(uint32 ID, ENodePinDirection Direction)
    {
        for (CEdNodeGraphPin* Pin : NodePins[uint32(Direction)])
        {
            if (Pin->PinID == ID)
            {
                return Pin;
            }
        }
        
        return nullptr;
    }

    CEdNodeGraphPin* CEdGraphNode::GetPinByIndex(uint32 Index, ENodePinDirection Direction)
    {
        return NodePins[uint32(Direction)][Index];
    }

    // Two live pins sharing an id self-link the editor's chain, so the id is salted until unique.
    bool CEdGraphNode::IsPinIDTaken(uint32 ID) const
    {
        for (const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins : NodePins)
        {
            for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (Pin.IsValid() && Pin->PinID == ID)
                {
                    return true;
                }
            }
        }
        return false;
    }

    CEdNodeGraphPin* CEdGraphNode::CreatePin(CClass* InClass, const FString& Name, ENodePinDirection Direction)
    {
        CEdNodeGraphPin* NewPin = NewObject<CEdNodeGraphPin>(InClass);
        NewPin->SetPinName(Name);

        uint32 ID = HashPinID(PinHashName, Name, Direction);
        for (uint32 Salt = 1; IsPinIDTaken(ID); ++Salt)
        {
            ID = HashPinID(PinHashName, Name + "#" + Format("{}", Salt), Direction);
        }
        NewPin->PinID = ID;
        NewPin->bInputPin = (Direction == ENodePinDirection::Input);
        NewPin->OwningNode = this;
        
        NodePins[uint32(Direction)].push_back(NewPin);

        return NewPin;
    }
}
