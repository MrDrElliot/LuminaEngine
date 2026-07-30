#include "EdGraphNode.h"

#include "EdNodeGraphPin.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/ObjectAllocator.h"
#include "Material/MaterialGraphTypes.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

namespace Lumina
{

    static uint32 HashPinID(const FString& NodeName, const FString& PinName, ENodePinDirection Direction)
    {
        FString Composite = NodeName + "_" + PinName + "_" + eastl::to_string((uint8)Direction);
        return Hash::GetHash32(Composite);
    }
    
    void CEdGraphNode::PostCreateCDO()
    {
        CObject::PostCreateCDO();
    }

    ImVec2 CEdGraphNode::GetMinNodeTitleBarSize() const
    {
        { const FStringView Name = GetNodeDisplayName(); return ImVec2(ImGui::CalcTextSize(Name.data(), Name.data() + Name.size()).x, 28); }
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

    void CEdGraphNode::DrawNodeTitleBar()
    {
        if (HasError())
        {
            ImGui::TextColored(ImVec4(255.0f, 0.0f, 0.0f, 255.f), LE_ICON_EXCLAMATION_THICK " %s", GetNodeDisplayName().data());
        }
        else
        {
            ImGui::TextUnformatted(GetNodeDisplayName().data());
        }
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

    // True when any pin already on this node (either direction) owns this id. Two live pins sharing an
    // id are begun as the same node-editor pin object, which self-links its m_PreviousPin chain and
    // hangs the editor's pin walks. Nodes with user-named pins (Custom Slang) can produce that, so the
    // id is salted until it is unique instead of trusting the name to be.
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

        uint32 ID = HashPinID(FullName, Name, Direction);
        for (uint32 Salt = 1; IsPinIDTaken(ID); ++Salt)
        {
            ID = HashPinID(FullName, Name + "#" + eastl::to_string(Salt), Direction);
        }
        NewPin->PinID = ID;
        NewPin->bInputPin = (Direction == ENodePinDirection::Input);
        NewPin->OwningNode = this;
        
        NodePins[uint32(Direction)].push_back(NewPin);

        return NewPin;
    }
}
