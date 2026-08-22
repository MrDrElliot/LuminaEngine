#include "AssetRefPropertyCustomization.h"

#include "imgui.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "FileSystem/FileSystem.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

namespace Lumina
{
    static constexpr ImVec2 GAssetRefButtonSize(42, 0);

    TSharedPtr<FAssetRefPropertyCustomization> FAssetRefPropertyCustomization::MakeInstance()
    {
        return MakeShared<FAssetRefPropertyCustomization>();
    }

    EPropertyChangeOp FAssetRefPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FAssetRef* Ref = static_cast<FAssetRef*>(Property->GetValuePtr());
        if (Ref == nullptr)
        {
            return EPropertyChangeOp::None;
        }

        // Kinds this slot accepts, from PROPERTY(AssetType="..."); empty meta == all kinds.
        TVector<ETextAssetKind> AllowedKinds;
        if (Property->Property != nullptr && Property->Property->HasMetadata(FName("AssetType")))
        {
            const FString& Meta = Property->Property->GetMetadata(FName("AssetType"));
            AllowedKinds = TextAsset::ParseAssetTypeMeta(FStringView(Meta.c_str(), Meta.size()));
        }
        else
        {
            AllowedKinds = TextAsset::ParseAssetTypeMeta(FStringView());
        }

        bool bWasChanged = false;

        ImGui::PushID(this);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        const FStringView ResolvedView = Ref->ResolvePath();
        const FString     ResolvedPath(ResolvedView.data(), ResolvedView.size());

        const float ComboArrowWidth = ImGui::GetFrameHeight();
        const float TotalWidth      = ImGui::GetContentRegionAvail().x;
        const float TextWidth       = TotalWidth - ComboArrowWidth - GAssetRefButtonSize.x * 3.0f - ImGui::GetStyle().ItemSpacing.x * 3.0f;

        ImGui::SetNextItemWidth(ImMax(TextWidth, 60.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

        FFixedString PathString(ResolvedPath.c_str(), ResolvedPath.size());
        ImGui::InputText("##AssetRefText", PathString.data(), PathString.max_size(), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);

        // Drop a file of an allowed kind from the content browser.
        if (ImGui::BeginDragDropTarget())
        {
            for (ETextAssetKind Kind : AllowedKinds)
            {
                FStringView Ext = TextAsset::ExtensionForKind(Kind);
                if (!Ext.empty() && Ext[0] == '.') Ext = Ext.substr(1);

                FFixedString Dropped;
                if (DragDrop::AcceptFile(Ext, Dropped))
                {
                    const FString NewPath(Dropped.c_str(), Dropped.size());
                    PendingMutation = [Ref, NewPath] { Ref->SetPath(NewPath); };
                    bWasChanged = true;
                    break;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (!ResolvedPath.empty())
        {
            ImGuiX::TextTooltip("{}", ResolvedPath);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 0);

        const ImVec2 DropdownSize = ImMax(ImVec2(220, 240), ImVec2(TextWidth, 320.0f));
        if (ImGui::BeginCombo("##AssetRefPick", "", ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview))
        {
            SearchFilter.Draw("##Search", DropdownSize.x - 30.0f);
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(200, 200), DropdownSize);

            if (ImGui::BeginChild("##OptList", DropdownSize, false, ImGuiChildFlags_NavFlattened))
            {
                FAssetRegistry& Registry = FAssetRegistry::Get();
                for (ETextAssetKind Kind : AllowedKinds)
                {
                    for (const FTextAssetData* Data : Registry.GetTextAssetsOfKind(Kind))
                    {
                        const FStringView CandView(Data->Path.c_str(), Data->Path.size());
                        if (!ImGuiX::PassSearchFilter(SearchFilter, Data->Path.c_str()))
                        {
                            continue;
                        }

                        if (ImGui::Selectable(Data->Path.c_str()))
                        {
                            const FString  NewPath(CandView.data(), CandView.size());
                            const FGuid    NewGuid = Data->Guid;
                            PendingMutation = [Ref, NewPath, NewGuid] { Ref->Set(NewPath, NewGuid); };
                            bWasChanged = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_OPEN_IN_NEW "##Open", GAssetRefButtonSize))
        {
            if (!ResolvedPath.empty())
            {
                VFS::PlatformOpen(ResolvedPath);
            }
        }
        ImGuiX::TextTooltip("Open in your native editor");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", GAssetRefButtonSize))
        {
            ImGui::SetClipboardText(ResolvedPath.c_str());
        }
        ImGuiX::TextTooltip("Copy the path to your clipboard");

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", GAssetRefButtonSize))
        {
            PendingMutation = [Ref] { Ref->Reset(); };
            bWasChanged = true;
        }
        ImGui::PopStyleColor(3);
        ImGuiX::TextTooltip("Clear the reference");

        ImGui::PopItemWidth();
        ImGui::PopID();

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

    void FAssetRefPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = nullptr;
        }
    }

    void FAssetRefPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
    }
}
