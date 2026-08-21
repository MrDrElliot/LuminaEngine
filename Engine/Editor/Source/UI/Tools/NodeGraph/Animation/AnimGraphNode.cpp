#include "AnimGraphNode.h"
#include "AnimationGraphCompiler.h"
#include "AnimationGraphNodeGraph.h"
#include "Core/Object/Class.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "imgui.h"

namespace Lumina
{
    // A shared width so the inline pin editors form a tidy right-aligned column on the node face.
    static constexpr float GInlinePinEditorWidth = 104.0f;

    CClass* CAnimGraphNode::GetSupportedGraphClass() const
    {
        return CAnimationGraphNodeGraph::StaticClass();
    }

    CAnimGraphPin* CAnimGraphNode::CreateAnimPin(const FString& Name, ENodePinDirection Direction, EAnimPinType Type, float DefaultValue)
    {
        CEdNodeGraphPin* Pin = CreatePin(CAnimGraphPin::StaticClass(), Name, Direction);
        CAnimGraphPin* AnimPin = static_cast<CAnimGraphPin*>(Pin);
        AnimPin->SetPinName(Name);
        AnimPin->SetPinType(Type);
        AnimPin->DefaultValue = DefaultValue;
        return AnimPin;
    }

    float CAnimGraphNode::GetValuePinDefault(const CAnimGraphPin* Pin) const
    {
        if (Pin == nullptr)
        {
            return 0.0f;
        }

        const FName Key(Pin->GetPinName());
        for (const FAnimGraphPinDefault& Entry : PinDefaults)
        {
            if (Entry.PinName == Key)
            {
                return Entry.Value;
            }
        }
        return Pin->DefaultValue;
    }

    void CAnimGraphNode::SetValuePinDefault(const CAnimGraphPin* Pin, float Value)
    {
        if (Pin == nullptr)
        {
            return;
        }

        const FName Key(Pin->GetPinName());
        for (FAnimGraphPinDefault& Entry : PinDefaults)
        {
            if (Entry.PinName == Key)
            {
                Entry.Value = Value;
                return;
            }
        }

        FAnimGraphPinDefault NewEntry;
        NewEntry.PinName = Key;
        NewEntry.Value   = Value;
        PinDefaults.push_back(NewEntry);
    }

    void CAnimGraphNode::MarkGraphDirty()
    {
        if (CPackage* Package = GetPackage())
        {
            Package->MarkDirty();
        }
    }

    void CAnimGraphNode::BindFloatPinEditor(CAnimGraphPin* Pin, float Speed, const char* Format)
    {
        if (Pin == nullptr)
        {
            return;
        }

        Pin->InlineEditor = [this, Pin, Speed, Format]()
        {
            float Value = GetValuePinDefault(Pin);
            ImGui::SetNextItemWidth(GInlinePinEditorWidth);
            if (ImGui::DragFloat("##v", &Value, Speed, 0.0f, 0.0f, Format))
            {
                SetValuePinDefault(Pin, Value);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                MarkGraphDirty();
            }
        };
    }

    void CAnimGraphNode::BindEnumPinEditor(CAnimGraphPin* Pin, const TVector<const char*>& Items,
                                           const TFunction<int()>& Get, const TFunction<void(int)>& Set)
    {
        if (Pin == nullptr)
        {
            return;
        }

        // A cycle button, since imgui-node-editor does not host popup windows reliably inside a node.
        Pin->InlineEditor = [this, Items, Get, Set]()
        {
            const int Count = (int)Items.size();
            if (Count == 0)
            {
                return;
            }

            const int Current = Get();
            const char* Label = (Current >= 0 && Current < Count) ? Items[Current] : "?";

            // A left-aligned label with room for a chevron reads as a value selector, not a plain button.
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            const bool bClicked = ImGui::Button(Label, ImVec2(GInlinePinEditorWidth, 0.0f));
            ImGui::PopStyleVar();

            const ImVec2 Min = ImGui::GetItemRectMin();
            const ImVec2 Max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(Max.x - 14.0f, Min.y + ImGui::GetStyle().FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), LE_ICON_MENU_DOWN);

            if (bClicked)
            {
                Set((Current + 1) % Count);
                MarkGraphDirty();
            }
        };
    }

    uint16 CAnimGraphNode::ResolvePoseInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler)
    {
        if (InputPin != nullptr && InputPin->HasConnection())
        {
            CEdNodeGraphPin* Source = InputPin->GetConnection(0);
            uint16 Register;
            if (Compiler.TryGetPinRegister(Source, Register))
            {
                return Register;
            }
        }

        // An unconnected pose input resolves to the bind pose, so a partial graph still evaluates.
        return Compiler.EmitRefPose();
    }

    int32 CAnimGraphNode::ResolveObjectInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler)
    {
        if (InputPin != nullptr && InputPin->HasConnection())
        {
            CEdNodeGraphPin* Source = InputPin->GetConnection(0);
            uint16 Register;
            if (Compiler.TryGetPinRegister(Source, Register))
            {
                return (int32)Register;
            }
        }

        // Unconnected means the node keeps its own statically assigned asset.
        return INDEX_NONE;
    }

    uint16 CAnimGraphNode::ResolveValueInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler)
    {
        if (InputPin != nullptr && InputPin->HasConnection())
        {
            CEdNodeGraphPin* Source = InputPin->GetConnection(0);
            uint16 Register;
            if (Compiler.TryGetPinRegister(Source, Register))
            {
                return Register;
            }
        }

        float Default = 0.0f;
        if (CAnimGraphPin* AnimPin = Cast<CAnimGraphPin>(InputPin))
        {
            Default = AnimPin->DefaultValue;
            if (CAnimGraphNode* Node = Cast<CAnimGraphNode>(AnimPin->GetOwningNode()))
            {
                Default = Node->GetValuePinDefault(AnimPin);
            }
        }
        return Compiler.EmitLoadConst(Default);
    }
}
