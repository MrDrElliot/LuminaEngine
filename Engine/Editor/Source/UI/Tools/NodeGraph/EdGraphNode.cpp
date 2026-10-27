#include "EdGraphNode.h"

#include "EdNodeGraph.h"
#include "EdNodeGraphPin.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectAllocator.h"
#include "Material/MaterialGraphTypes.h"
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

    void CEdGraphNode::NotifyValueEdited()
    {
        if (OwningGraph == nullptr)
        {
            return;
        }

        // A comment changes nothing the graph compiles, so its edit is recorded without asking for a recompile.
        if (IsCommentNode())
        {
            OwningGraph->NotifyCosmeticChange();
        }
        else
        {
            OwningGraph->NotifyContentChanged();
        }
    }

    void CEdGraphNode::NotifySubGraphCreated()
    {
        if (OwningGraph != nullptr)
        {
            OwningGraph->NotifyContentChanged();
            OwningGraph->CommitEditIntoLastStep();
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
            ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.88f, 1.0f), LE_ICON_EXCLAMATION_THICK " %s", Title.c_str());
        }
        else if (HasWarning())
        {
            ImGui::TextColored(ImVec4(0.14f, 0.11f, 0.03f, 1.0f), LE_ICON_ALERT " %s", Title.c_str());
        }
        else
        {
            ImGui::TextUnformatted(Title.c_str());
        }
    }

    CEdNodeGraphPin* CEdGraphNode::GetRerouteSourcePin() const
    {
        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Inputs = GetInputPins();
        return Inputs.empty() ? nullptr : Inputs[0].Get();
    }

    CEdNodeGraphPin* CEdGraphNode::GetPin(uint32 ID, ENodePinDirection Direction)
    {
        auto& Pins = NodePins[uint32(Direction)];
        const auto PinItr = Algo::Find(Pins, ID, &CEdNodeGraphPin::PinID);

        return PinItr != Pins.end() ? PinItr->Get() : nullptr;
    }

    CEdNodeGraphPin* CEdGraphNode::GetPinByIndex(uint32 Index, ENodePinDirection Direction)
    {
        return NodePins[uint32(Direction)][Index].Get();
    }

    // Two live pins sharing an id self-link the editor's chain, so the id is salted until unique.
    bool CEdGraphNode::IsPinIDTaken(uint32 ID) const
    {
        for (const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins : NodePins)
        {
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
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
