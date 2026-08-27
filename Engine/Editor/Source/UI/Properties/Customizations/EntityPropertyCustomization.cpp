#include "EntityPropertyCustomization.h"

#include "imgui.h"
#include "World/ECS/Registry.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Lumina.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "World/World.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // Integral id of an unset entity reference.
        const uint32 GNoneEntityId = static_cast<uint32>((static_cast<ECS::FEntity>(ECS::NullEntity)).Value);

        FFixedString MakeEntityLabel(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FFixedString Label;
            if (const SNameComponent* Name = Registry.TryGet<SNameComponent>(Entity))
            {
                Label.append(Name->Name.c_str());
            }
            else
            {
                Label.append("Entity");
            }
            AppendFormat(Label, " ({})", static_cast<uint32>((Entity).Value));
            return Label;
        }
    }

    FEntityPropertyCustomization::~FEntityPropertyCustomization()
    {
        // The broker is held shared, so withdrawing here is safe even after the tool that served it went.
        if (PickBroker && PickBroker->IsActiveFor(reinterpret_cast<uint64>(this)))
        {
            PickBroker->Cancel();
        }
    }

    EPropertyChangeOp FEntityPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        const FWorldEditContext* WorldCtx = Args.Context.Get<FWorldEditContext>();
        CWorld* World = (WorldCtx != nullptr) ? WorldCtx->World : nullptr;

        const FEntityPickContext* PickCtx = Args.Context.Get<FEntityPickContext>();
        PickBroker = (PickCtx != nullptr) ? PickCtx->Broker : nullptr;

        // No world to resolve against (e.g. an asset editor); show the raw id, read-only.
        if (World == nullptr)
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            int Value = static_cast<int>(CachedValue);
            ImGui::BeginDisabled(true);
            ImGui::InputInt("##entity", &Value, 0, 0, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
            ImGui::PopItemWidth();
            return EPropertyChangeOp::None;
        }

        const uint64 Token = reinterpret_cast<uint64>(this);
        bool bChanged = false;

        // Apply an entity clicked in the viewport since last frame (eyedropper result).
        uint32 PickedEntity = 0;
        if (PickBroker && PickBroker->ConsumeResult(Token, PickedEntity))
        {
            CachedValue = PickedEntity;
            bChanged = true;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Index 0 is always "None"; the rest are the same named entities the outliner shows.
        TVector<ECS::FEntity> Candidates;
        Candidates.push_back(ECS::NullEntity);

        const ECS::FEntity CurrentEntity = static_cast<ECS::FEntity>(CachedValue);
        const bool bHasCurrent = (CachedValue != GNoneEntityId) && Registry.IsValid(CurrentEntity);

        int32 CurrentIndex = 0; // default to None
        for (ECS::FEntity Entity : Registry.View<SNameComponent>(ECS::TExclude<FHideInSceneOutliner>{}))
        {
            if (bHasCurrent && Entity == CurrentEntity)
            {
                CurrentIndex = static_cast<int32>(Candidates.size());
            }
            Candidates.push_back(Entity);
        }

        const FFixedString Preview = bHasCurrent ? MakeEntityLabel(Registry, CurrentEntity) : FFixedString("None");

        // Leave room on the right for the square eyedropper button.
        const float LineHeight = ImGui::GetFrameHeight();
        const float Spacing    = ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - LineHeight - Spacing);

        const int32 Picked = ImGuiX::SearchableCombo("##entity", Preview.c_str(), static_cast<int32>(Candidates.size()), CurrentIndex,
            [&Registry, &Candidates](int32 Index) -> FFixedString
            {
                if (Index == 0)
                {
                    return FFixedString("None");
                }
                return MakeEntityLabel(Registry, Candidates[Index]);
            }, LE_ICON_CUBE);

        ImGui::PopItemWidth();

        if (Picked != INDEX_NONE)
        {
            CachedValue = (Picked == 0) ? GNoneEntityId : static_cast<uint32>((Candidates[Picked]).Value);
            bChanged = true;
        }

        // Arms a viewport pick, and clicking again or pressing Esc in the viewport cancels it.
        ImGui::SameLine(0.0f, Spacing);
        const bool bPicking = PickBroker && PickBroker->IsActiveFor(Token);
        if (bPicking)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(LE_ICON_EYEDROPPER "##pickentity", ImVec2(LineHeight, LineHeight)))
        {
            if (PickBroker)
            {
                bPicking ? PickBroker->Cancel() : PickBroker->Request(Token);
            }
        }
        if (bPicking)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(bPicking ? "Click an entity in the viewport (Esc to cancel)" : "Pick an entity from the viewport");
        }

        return bChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FEntityPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(CachedValue);
    }

    void FEntityPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&CachedValue);
    }
}
