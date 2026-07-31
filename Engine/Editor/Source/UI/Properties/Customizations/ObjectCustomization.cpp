#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/EditorUI.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>
#include <Containers/Array.h>
#include <Containers/String.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Core/Object/ObjectHandleTyped.h>
#include <Core/Reflection/PropertyCustomization/PropertyCustomization.h>
#include <Core/Templates/Optional.h>
#include <Memory/SmartPtr.h>
#include <imgui_internal.h>
#include <LuminaEditor.h>
#include "Core/Object/Package/Package.h"
#include "thumbnails/thumbnailmanager.h"

namespace Lumina
{
    static constexpr ImVec2 GButtonSize(42, 0);

    EPropertyChangeOp FCObjectPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FObjectProperty* ObjectProperty = static_cast<FObjectProperty*>(Property->Property);
        
        bool bWasChanged = false;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        ImGui::PushID(this);
        if (ImGui::BeginChild("OP", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            const auto& Style = ImGui::GetStyle();

            TObjectPtr<CObject> HardObject = Object.Lock();

            const char* Label = HardObject.IsValid() ? Object.Get()->GetName().c_str() : "<None>";
            ImGui::BeginDisabled(Object == nullptr);

            TOptional<ImTextureRef> ButtonTexture;
            if (Object.IsValid() && HardObject->GetPackage())
            {
                if (FPackageThumbnail* Thumbnail = CThumbnailManager::Get().GetThumbnailForPackage(HardObject->GetPackage()->GetName()))
                {
                    ButtonTexture = ImGuiX::ToImTextureRef(Thumbnail->LoadedImage);
                }
            }

            if (!ButtonTexture.has_value())
            {
                ButtonTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png");
            }
            
            ImGui::ImageButton(Label, ButtonTexture.value(), ImVec2(64, 64));
            ImGui::EndDisabled();

            // Asset drop target: any drag of an asset whose class is-a property's
            // declared class binds the property to that asset.
            if (ImGui::BeginDragDropTarget())
            {
                if (CObject* Dropped = DragDrop::AcceptAssetOfClass(ObjectProperty->GetPropertyClass()))
                {
                    Object = Dropped;
                    bWasChanged = true;
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (HardObject)
                {
                    static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI())->OpenAssetEditor(HardObject->GetGUID());
                }
            }

            ImGui::SameLine();

            ImGui::BeginGroup();

            float const ComboArrowWidth = ImGui::GetFrameHeight();
            float const TotalPathWidgetWidth = ImGui::GetContentRegionAvail().x;
            float const TextWidgetWidth = TotalPathWidgetWidth - ComboArrowWidth;

            ImGui::SetNextItemWidth(TextWidgetWidth);
            
            const bool bHasObject = Object != nullptr;

            FFixedString PathString = bHasObject ? Object.Get()->GetName().c_str() : "<None>";
        
            ImGui::PushStyleColor(ImGuiCol_Text, bHasObject ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 0.19f, 0.19f, 1.0f));
        
            ImGui::InputText("##ObjectPathText", PathString.data(), PathString.max_size(), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
        
            ImGuiX::TextTooltip("{}", PathString);
        
            ImGui::PopStyleColor();

            const ImVec2 ComboDropDownSize = ImMax(ImVec2(200, 200), ImVec2(TextWidgetWidth, 300.0f));

            ImGui::SameLine(0, 0);
        
            bool bComboOpen = ImGui::BeginCombo("##ObjectPath", "", ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview);

            if (bComboOpen)
            {
                SearchFilter.Draw("##Search", ComboDropDownSize.x - 30.0f);
                if (!SearchFilter.IsActive())
                {
                    ImDrawList* DrawList = ImGui::GetWindowDrawList();
                    ImVec2 TextPos = ImGui::GetItemRectMin();
                    TextPos.x += Style.FramePadding.x + 2.0f;
                    TextPos.y += Style.FramePadding.y;
                    DrawList->AddText(TextPos, IM_COL32(100, 100, 110, 255), LE_ICON_FILE_SEARCH " Search Assets...");
                }
                
                ImGui::SameLine();
                ImGui::Button(LE_ICON_FILTER, ImVec2(30.0f, 0.0f));
                ImGui::SetNextWindowSizeConstraints(ImVec2(200, 200), ComboDropDownSize);

                if (ImGui::BeginChild("##OptList", ComboDropDownSize, false, ImGuiChildFlags_NavFlattened))
                {
                    TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                    {
                        CClass* DataClass = FindObject<CClass>(Data.AssetClass);
                        CClass* PropertyClass = ObjectProperty->GetPropertyClass();
                        return DataClass->IsChildOf(PropertyClass);
                    });
                    
                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 4));
                    if (ImGui::BeginTable("##AssetTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner))
                    {
                        ImGui::TableSetupColumn("##Thumb", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                        ImGui::TableSetupColumn("##Name",  ImGuiTableColumnFlags_WidthStretch);
                        
                        for (const FAssetData* Asset : Assets)
                        {
                            if (!SearchFilter.PassFilter(Asset->AssetName.c_str()))
                            {
                                continue;
                            }
                        
                            ImGui::PushID(Asset);
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, 42.0f);
                            ImGui::TableSetColumnIndex(0);
                            
                            bool bSelected = ImGui::Selectable("##sel", false, 
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 42.0f));
                            
                            ImGuiX::TextTooltip("{}", Asset->Path);
                            
                            ImGui::SameLine();
                            
                            if (FPackageThumbnail* Thumbnail = CThumbnailManager::Get().GetThumbnailForPackage(Asset->Path))
                            {
                                ImGui::Image(ImGuiX::ToImTextureRef(Thumbnail->LoadedImage), ImVec2(42, 42));
                            }
                            else
                            {
                                ImGui::Image(ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png"), ImVec2(42, 42));
                            }
                            
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(Asset->AssetName.c_str());

                            if (bSelected)
                            {
                                Object = LoadObject<CObject>(Asset->AssetGUID);
                                ImGui::CloseCurrentPopup();
                                bWasChanged = true;
                            }
                        
                            ImGui::PopID();
                        }
                        
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();
                }
                
                ImGui::EndChild();
                ImGui::EndCombo();
            }
        
            FEditorUI* EditorUI = static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());

            // "Use the asset selected in the Content Browser". Outside the disabled block below on
            // purpose: this is most useful when the slot is EMPTY, which is exactly when everything
            // else here is greyed out. Enabled only for a selection this property could actually hold.
            const FAssetData* BrowserSelection = EditorUI != nullptr ? EditorUI->GetContentBrowserSelectedAsset() : nullptr;
            bool bSelectionFits = false;
            if (BrowserSelection != nullptr)
            {
                CClass* SelectedClass = FindObject<CClass>(BrowserSelection->AssetClass);
                bSelectionFits = SelectedClass != nullptr && SelectedClass->IsChildOf(ObjectProperty->GetPropertyClass());
            }

            ImGui::BeginDisabled(!bSelectionFits);
            if (ImGui::Button(LE_ICON_ARROW_LEFT_CIRCLE "##UseSelected", GButtonSize))
            {
                if (CObject* Loaded = LoadObject<CObject>(BrowserSelection->AssetGUID))
                {
                    Object = Loaded;
                    bWasChanged = true;
                }
            }
            ImGui::EndDisabled();

            ImGuiX::TextTooltip("{}", bSelectionFits
                ? "Use the asset selected in the Content Browser."
                : "Select a compatible asset in the Content Browser to use it here.");

            ImGui::SameLine();

            ImGui::BeginDisabled(Object == nullptr);

            // Reveal in the Content Browser -- the inverse of the button above.
            if (ImGui::Button(LE_ICON_MAGNIFY "##Browse", GButtonSize))
            {
                if (HardObject && HardObject->GetPackage() && EditorUI != nullptr)
                {
                    const FString PackageName = HardObject->GetPackage()->GetName().ToString();
                    EditorUI->BrowseToAsset(FStringView(PackageName.c_str(), PackageName.size()));
                }
            }
            ImGuiX::TextTooltip("{}", "Show this asset in the Content Browser.");

            ImGui::SameLine();

            if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", GButtonSize))
            {
                ImGui::SetClipboardText(HardObject->GetName().c_str());
            }

            ImGui::SameLine();
        
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));

            if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", GButtonSize))
            {
                Object.Reset();
                bWasChanged = true;
            }
        
            ImGui::PopStyleColor(3);


            ImGui::EndDisabled();
            ImGui::EndGroup();
        }
        ImGui::EndChild();

        ImGui::PopID();

        ImGui::PopItemWidth();

        if (bWasChanged)
        {
            // A transaction from a prior change is still settling; fold this one into it.
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            // Open the transaction now: StartChangeCallback snapshots the old value
            // before UpdatePropertyValue writes the new one.
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

    void FCObjectPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        TObjectPtr<CObject> Value = Object.Lock();
        Property->SetValue(Value);
    }

    void FCObjectPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        TObjectPtr<CObject> Value;
        Property->GetValue(&Value);
        Object = Value;
    }
}
