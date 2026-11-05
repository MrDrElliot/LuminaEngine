#include "AssetIdPropertyCustomization.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"

#include "LuminaEditor.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/EditorUI.h"

namespace Lumina
{
    namespace
    {
        // The id an asset has, or an invalid one when its class is in no group or not the one the field wants.
        SAssetId IdOf(const FAssetData* Data, FName RequiredGroup)
        {
            if (Data == nullptr)
            {
                return {};
            }
            const FName Group = FAssetManager::Get().GetGroupOfClass(Data->AssetClass);
            if (Group.IsNone() || (!RequiredGroup.IsNone() && Group != RequiredGroup))
            {
                return {};
            }
            return SAssetId{ Group, Data->AssetName };
        }

        FString Describe(const SAssetId& Id)
        {
            return FString(*Id.Group) + ":" + *Id.Name;
        }
    }

    TSharedPtr<FAssetIdPropertyCustomization> FAssetIdPropertyCustomization::MakeInstance()
    {
        return MakeShared<FAssetIdPropertyCustomization>();
    }

    EPropertyChangeOp FAssetIdPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        SAssetId* Id = static_cast<SAssetId*>(Property->GetValuePtr());
        if (Id == nullptr)
        {
            return EPropertyChangeOp::None;
        }

        FAssetManager& Manager = FAssetManager::Get();
        const FName RequiredGroup = Property->Property != nullptr && Property->Property->HasMetadata("Group")
            ? FName(Property->Property->GetMetadata("Group")) : FName();

        const bool        bHasId   = Id->IsValid();
        const FGuid       Resolved = bHasId ? Manager.Resolve(*Id) : FGuid();
        const FAssetData* Data     = Resolved.IsValid() ? FAssetRegistry::Get().GetAssetByGUID(Resolved) : nullptr;
        const bool        bMissing = bHasId && Data == nullptr;

        bool bWasChanged = false;
        auto Assign = [&](const SAssetId& NewId)
        {
            PendingMutation = [Id, NewId] { *Id = NewId; };
            bWasChanged = true;
        };

        FEditorUI* EditorUI = static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushID(this);
        ImGui::BeginGroup();

        const FString Preview = !bHasId  ? FString("<None>")
                              : bMissing ? FString(LE_ICON_ALERT " ") + Describe(*Id)
                              :            Describe(*Id);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_Text, bHasId && !bMissing ? AssetPickerFilter::kMutedText : AssetPickerFilter::kEmptyText);
        const bool bOpen = ImGui::BeginCombo("##Pick", *Preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft);
        ImGui::PopStyleColor();

        // A grouped asset dragged from the content browser fills the slot without loading anything.
        if (ImGui::BeginDragDropTarget())
        {
            if (const DragDrop::FPayload* Payload = DragDrop::PeekPayload(); Payload != nullptr && Payload->Kind == DragDrop::EPayloadKind::Asset)
            {
                const SAssetId Dropped = IdOf(FAssetRegistry::Get().GetAssetByPath(FStringView(*Payload->AssetPath, Payload->AssetPath.Len())), RequiredGroup);
                if (Dropped.IsValid() && ImGui::AcceptDragDropPayload(DragDrop::GImGuiPayloadType) != nullptr)
                {
                    Assign(Dropped);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (!bOpen)
        {
            const FString Tip = bMissing         ? FString("Nothing in group ") + *Id->Group + " is named " + *Id->Name
                              : Data != nullptr  ? FString(*Data->Path)
                              : RequiredGroup.IsNone() ? FString("No asset. Pick one, or drop a grouped asset here.")
                              :                  FString("No asset. Pick a ") + *RequiredGroup + ", or drop one here.";
            ImGuiX::TextTooltip("{}", Tip);
        }
        else
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            SearchFilter.Draw("##Search", -1.0f);

            if (ImGui::Selectable(LE_ICON_CLOSE " None", !bHasId))
            {
                Assign({});
            }

            TVector<FName> Groups;
            if (RequiredGroup.IsNone())
            {
                Manager.GetGroups(Groups);
                Algo::Sort(Groups, [](const FName& A, const FName& B) { return strcmp(*A, *B) < 0; });
            }
            else
            {
                Groups.Add(RequiredGroup);
            }

            uint32 Shown = 0;
            for (const FName& Group : Groups)
            {
                TVector<SAssetId> Members;
                Manager.GetGroup(Group, Members);
                Algo::Sort(Members, [](const SAssetId& A, const SAssetId& B) { return strcmp(*A.Name, *B.Name) < 0; });

                bool bHeader = false;
                for (const SAssetId& Member : Members)
                {
                    const FString Label = Describe(Member);
                    if (!ImGuiX::PassSearchFilter(SearchFilter, *Label))
                    {
                        continue;
                    }
                    if (!bHeader)
                    {
                        ImGui::SeparatorText(*Group);
                        bHeader = true;
                    }

                    ImGui::PushID(*Label);
                    const FString Item = FString(LE_ICON_TAG_OUTLINE " ") + *Member.Name;
                    if (ImGui::Selectable(*Item, Member == *Id))
                    {
                        Assign(Member);
                    }
                    ImGui::PopID();
                    ++Shown;
                }
            }

            if (Shown == 0)
            {
                ImGui::TextDisabled("%s", Groups.IsEmpty() ? "No asset class declares REFLECT(Group = ...)." : "Nothing matches.");
            }
            ImGui::EndCombo();
        }

        // Outside any disabled block, since this is most useful exactly when the slot is empty.
        const SAssetId Selected = IdOf(EditorUI != nullptr ? EditorUI->GetContentBrowserSelectedAsset() : nullptr, RequiredGroup);
        ImGui::BeginDisabled(!Selected.IsValid());
        if (ImGui::Button(LE_ICON_ARROW_LEFT_CIRCLE "##UseSelected", AssetPickerFilter::kButtonSize))
        {
            Assign(Selected);
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", Selected.IsValid() ? "Use the asset selected in the Content Browser." : "Select a grouped asset in the Content Browser to use it here.");

        ImGui::SameLine();
        ImGui::BeginDisabled(Data == nullptr);
        if (ImGui::Button(LE_ICON_MAGNIFY "##Browse", AssetPickerFilter::kButtonSize) && EditorUI != nullptr && Data != nullptr)
        {
            EditorUI->BrowseToAsset(FStringView(*Data->Path, Data->Path.Len()));
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", "Show this asset in the Content Browser.");

        ImGui::SameLine();
        ImGui::BeginDisabled(!bHasId);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", AssetPickerFilter::kButtonSize))
        {
            Assign({});
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", "Clear the id.");

        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::PopItemWidth();

        if (bWasChanged)
        {
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }

        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }

        return EPropertyChangeOp::None;
    }

    void FAssetIdPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = nullptr;
        }
    }
}
