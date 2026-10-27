#include "ParticlePin.h"
#include "ParticleGraphNode.h"
#include "Core/Object/Cast.h"
#include <imgui.h>

namespace Lumina
{
    FVector4& CParticleInput::GetDefault()
    {
        CParticleGraphNode* Node = Cast<CParticleGraphNode>(GetOwningNode());
        if (Node == nullptr)
        {
            return InitialDefault;
        }

        const auto& Inputs = Node->GetInputPins();
        int32 Slot = 0;
        while (Slot < (int32)Inputs.size() && Inputs[Slot].Get() != this)
        {
            ++Slot;
        }

        // A saved value wins over the pin's initial one, and pins past the saved count start from theirs.
        for (int32 Missing = (int32)Node->PinDefaults.size(); Missing <= Slot && Missing < (int32)Inputs.size(); ++Missing)
        {
            const CParticleInput* Pin = Cast<CParticleInput>(Inputs[Missing].Get());
            Node->PinDefaults.push_back(Pin != nullptr ? Pin->InitialDefault : FVector4(0.0f));
        }
        return Slot < (int32)Node->PinDefaults.size() ? Node->PinDefaults[Slot] : InitialDefault;
    }

    float CParticleInput::DrawPin()
    {
        if (!ShouldDrawEditor())
        {
            ImGui::Dummy(ImVec2(1.5f, 1.5f));
            return 1.5f;
        }

        FVector4& Value = GetDefault();
        float Width = 1.5f;
        switch (PinType)
        {
        case EParticlePinType::Float:
            Width = 70.0f;
            ImGui::SetNextItemWidth(Width);
            ImGui::DragFloat("##V", &Value.x, 0.01f);
            break;
        case EParticlePinType::Float2:
            Width = 120.0f;
            ImGui::SetNextItemWidth(Width);
            ImGui::DragFloat2("##V", &Value.x, 0.01f);
            break;
        case EParticlePinType::Float3:
            Width = 160.0f;
            ImGui::SetNextItemWidth(Width);
            ImGui::DragFloat3("##V", &Value.x, 0.01f);
            break;
        case EParticlePinType::Float4:
            Width = 200.0f;
            ImGui::SetNextItemWidth(Width);
            ImGui::ColorEdit4("##V", &Value.x);
            break;
        }

        // The value is baked into the generated code, so finishing an edit asks for a recompile.
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            GetOwningNode()->NotifyValueEdited();
        }
        return Width;
    }

    float CParticleOutput::DrawPin()
    {
        ImGui::Dummy(ImVec2(1.5f, 1.5f));
        return 1.5f;
    }
}
