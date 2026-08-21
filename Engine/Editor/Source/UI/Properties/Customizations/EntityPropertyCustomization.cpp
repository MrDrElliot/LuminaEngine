#include "EntityPropertyCustomization.h"

#include "imgui.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Lumina.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/EntityPropertyContext.h"
#include "World/World.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // Integral id of an unset entity reference.
        const uint32 GNoneEntityId = static_cast<uint32>(entt::to_integral(static_cast<entt::entity>(entt::null)));

        FFixedString MakeEntityLabel(FEntityRegistry& Registry, entt::entity Entity)
        {
            FFixedString Label;
            if (const SNameComponent* Name = Registry.try_get<SNameComponent>(Entity))
            {
                Label.append(Name->Name.c_str());
            }
            else
            {
                Label.append("Entity");
            }
            AppendFormat(Label, " ({})", static_cast<uint32>(entt::to_integral(Entity)));
            return Label;
        }
    }

    FEntityPropertyCustomization::~FEntityPropertyCustomization()
    {
        if (IsEntityPickActiveFor(reinterpret_cast<uint64>(this)))
        {
            CancelEntityPick();
        }
    }

    EPropertyChangeOp FEntityPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        CWorld* World = GetEntityPropertyContextWorld();

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
        if (ConsumeEntityPickResult(Token, PickedEntity))
        {
            CachedValue = PickedEntity;
            bChanged = true;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Index 0 is always "None"; the rest are the same named entities the outliner shows.
        TVector<entt::entity> Candidates;
        Candidates.push_back(entt::null);

        const entt::entity CurrentEntity = static_cast<entt::entity>(CachedValue);
        const bool bHasCurrent = (CachedValue != GNoneEntityId) && Registry.valid(CurrentEntity);

        int32 CurrentIndex = 0; // default to None
        for (entt::entity Entity : Registry.view<SNameComponent>(entt::exclude<FHideInSceneOutliner>))
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
            CachedValue = (Picked == 0) ? GNoneEntityId : static_cast<uint32>(entt::to_integral(Candidates[Picked]));
            bChanged = true;
        }

        // Arms a viewport pick, and clicking again or pressing Esc in the viewport cancels it.
        ImGui::SameLine(0.0f, Spacing);
        const bool bPicking = IsEntityPickActiveFor(Token);
        if (bPicking)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(LE_ICON_EYEDROPPER "##pickentity", ImVec2(LineHeight, LineHeight)))
        {
            bPicking ? CancelEntityPick() : RequestEntityPick(Token);
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
