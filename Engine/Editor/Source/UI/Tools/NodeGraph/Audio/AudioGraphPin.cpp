#include "EditorPCH.h"
#include "AudioGraphPin.h"

#include "Core/Object/Cast.h"
#include "Core/Object/Object.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include <imgui.h>

namespace Lumina
{
    uint32 GetAudioGraphTypeColor(EAudioGraphType Type)
    {
        switch (Type)
        {
        case EAudioGraphType::Audio:   return IM_COL32(120, 200, 255, 255);
        case EAudioGraphType::Float:   return IM_COL32(160, 230, 130, 255);
        case EAudioGraphType::Int32:   return IM_COL32(120, 200, 190, 255);
        case EAudioGraphType::Bool:    return IM_COL32(220, 110, 110, 255);
        case EAudioGraphType::Trigger: return IM_COL32(255, 200, 90, 255);
        case EAudioGraphType::Wave:    return IM_COL32(210, 140, 240, 255);
        default:                       return IM_COL32(200, 200, 200, 255);
        }
    }

    void* CAudioGraphPin::GetDefaultValuePtr() const
    {
        if (DefaultProperty == nullptr || OwningNode == nullptr)
        {
            return nullptr;
        }

        return DefaultProperty->GetValuePtr<uint8>(OwningNode);
    }

    float CAudioGraphPin::ReadFloatDefault() const
    {
        const void* Value = GetDefaultValuePtr();
        return Value != nullptr ? *static_cast<const float*>(Value) : 0.0f;
    }

    int32 CAudioGraphPin::ReadIntDefault() const
    {
        const void* Value = GetDefaultValuePtr();
        if (Value == nullptr)
        {
            return 0;
        }

        // An enum member is as wide as its underlying type, so reading it as int32 would take neighbors.
        if (FEnumProperty* EnumProperty = dynamic_cast<FEnumProperty*>(DefaultProperty))
        {
            if (FNumericProperty* Inner = EnumProperty->GetInnerProperty())
            {
                return (int32)Inner->GetSignedIntPropertyValue(Value);
            }
        }

        return *static_cast<const int32*>(Value);
    }

    bool CAudioGraphPin::ReadBoolDefault() const
    {
        const void* Value = GetDefaultValuePtr();
        return Value != nullptr ? *static_cast<const bool*>(Value) : false;
    }

    CObject* CAudioGraphPin::ReadObjectDefault() const
    {
        void* Value = GetDefaultValuePtr();
        return Value != nullptr ? *static_cast<CObject**>(Value) : nullptr;
    }

    bool CAudioGraphPin::HasInlineEditor() const
    {
        if (!bInputPin || HasConnection() || DefaultProperty == nullptr)
        {
            return false;
        }

        return PinType == EAudioGraphType::Float
            || PinType == EAudioGraphType::Int32
            || PinType == EAudioGraphType::Bool;
    }

    float CAudioGraphPin::DrawPin()
    {
        if (!ShouldDrawEditor() || !HasInlineEditor())
        {
            ImGui::Dummy(ImVec2(1.5f, 1.5f));
            return 1.5f;
        }

        void* Value = GetDefaultValuePtr();
        if (Value == nullptr)
        {
            ImGui::Dummy(ImVec2(1.5f, 1.5f));
            return 1.5f;
        }

        switch (PinType)
        {
        case EAudioGraphType::Float:
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("##V", static_cast<float*>(Value), 0.01f))
            {
                OwningNode->NotifyValueEdited();
            }
            return 90.0f;

        case EAudioGraphType::Int32:
            if (FEnumProperty* EnumProperty = dynamic_cast<FEnumProperty*>(DefaultProperty))
            {
                CEnum* Enum = EnumProperty->GetEnum();
                FNumericProperty* Inner = EnumProperty->GetInnerProperty();

                if (Enum != nullptr && Inner != nullptr)
                {
                    const int64 Current = Inner->GetSignedIntPropertyValue(Value);

                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::BeginCombo("##E", Enum->GetNameAtValue((uint64)Current).c_str()))
                    {
                        for (int64 Index = 0; Index < (int64)Enum->Names.size(); ++Index)
                        {
                            const int64 EntryValue = (int64)Enum->GetValueAtIndex(Index);

                            if (ImGui::Selectable(Enum->GetNameAtIndex(Index).c_str(), EntryValue == Current))
                            {
                                Inner->SetIntPropertyValue(Value, EntryValue);
                                OwningNode->NotifyValueEdited();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    return 110.0f;
                }
            }

            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragInt("##V", static_cast<int32*>(Value)))
            {
                OwningNode->NotifyValueEdited();
            }
            return 90.0f;

        case EAudioGraphType::Bool:
            if (ImGui::Checkbox("##V", static_cast<bool*>(Value)))
            {
                OwningNode->NotifyValueEdited();
            }
            return 20.0f;

        default:
            break;
        }

        ImGui::Dummy(ImVec2(1.5f, 1.5f));
        return 1.5f;
    }
}
