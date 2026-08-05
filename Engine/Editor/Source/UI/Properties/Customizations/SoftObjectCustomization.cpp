#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>
#include <Containers/Array.h>
#include <Containers/String.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Core/Object/Package/Package.h>
#include <imgui_internal.h>
#include "Thumbnails/ThumbnailManager.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"

namespace Lumina
{
    static constexpr ImVec2 GSoftButtonSize(42, 0);

    // Trailing path segment for a compact label ("/Game/Maps/Main" -> "Main").
    static FFixedString SoftLeafName(FStringView PathView)
    {
        if (PathView.empty())
        {
            return FFixedString("<None>");
        }
        size_t Slash = PathView.find_last_of('/');
        FStringView Leaf = (Slash == FStringView::npos) ? PathView : PathView.substr(Slash + 1);
        return FFixedString(Leaf.data(), Leaf.size());
    }

    EPropertyChangeOp FSoftObjectPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FSoftObjectProperty* SoftProperty = static_cast<FSoftObjectProperty*>(Property->Property);
        CClass* PropertyClass = SoftProperty->GetPropertyClass();

        bool bWasChanged = false;

        const FStringView PathView = Path.GetPath();
        const bool bHasPath = !PathView.empty();
        const FName PathName = bHasPath ? FName(FString(PathView.data(), PathView.size())) : NAME_None;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushID(this);
        if (ImGui::BeginChild("SOP", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            const auto& Style = ImGui::GetStyle();

            // Thumbnail (resolved from the registry by path; never loads the asset).
            TOptional<ImTextureRef> ButtonTexture;
            if (bHasPath)
            {
                if (FPackageThumbnail* Thumbnail = CThumbnailManager::Get().GetThumbnailForPackage(PathName))
                {
                    ButtonTexture = ImGuiX::ToImTextureRef(Thumbnail->LoadedImage);
                }
            }
            if (!ButtonTexture.has_value())
            {
                ButtonTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png");
            }

            const FFixedString Leaf = SoftLeafName(PathView);
            ImGui::BeginDisabled(!bHasPath);
            ImGui::ImageButton(Leaf.c_str(), ButtonTexture.value(), ImVec2(64, 64));
            ImGui::EndDisabled();

            // Drop target: bind to a dragged asset of the property's class (by path, no load).
            if (ImGui::BeginDragDropTarget())
            {
                if (CObject* Dropped = DragDrop::AcceptAssetOfClass(PropertyClass))
                {
                    if (Dropped->GetPackage())
                    {
                        const FString DroppedPath = Dropped->GetPackage()->GetName().ToString();
                        Path.SetPath(FStringView(DroppedPath.c_str(), DroppedPath.size()));
                        bWasChanged = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::SameLine();
            ImGui::BeginGroup();

            const float ComboArrowWidth = ImGui::GetFrameHeight();
            const float TotalPathWidgetWidth = ImGui::GetContentRegionAvail().x;
            const float TextWidgetWidth = TotalPathWidgetWidth - ComboArrowWidth;

            ImGui::SetNextItemWidth(TextWidgetWidth);

            FFixedString PathString = bHasPath ? FFixedString(PathView.data(), PathView.size()) : FFixedString("<None>");
            ImGui::PushStyleColor(ImGuiCol_Text, bHasPath ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 0.19f, 0.19f, 1.0f));
            ImGui::InputText("##SoftObjectPathText", PathString.data(), PathString.max_size(), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
            ImGuiX::TextTooltip("{}", PathString);
            ImGui::PopStyleColor();

            const ImVec2 ComboDropDownSize = ImMax(ImVec2(200, 200), ImVec2(TextWidgetWidth, 300.0f));

            ImGui::SameLine(0, 0);
            if (ImGui::BeginCombo("##SoftObjectPath", "", ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview))
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
                AssetPickerFilter::DrawFilterButton(30.0f);
                ImGui::SetNextWindowSizeConstraints(ImVec2(200, 200), ComboDropDownSize);

                if (ImGui::BeginChild("##OptList", ComboDropDownSize, false, ImGuiChildFlags_NavFlattened))
                {
                    TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                    {
                        CClass* DataClass = FindObject<CClass>(Data.AssetClass);
                        return DataClass && PropertyClass && DataClass->IsChildOf(PropertyClass);
                    });

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 4));
                    if (ImGui::BeginTable("##AssetTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner))
                    {
                        ImGui::TableSetupColumn("##Thumb", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                        ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                        for (const FAssetData* Asset : Assets)
                        {
                            if (!SearchFilter.PassFilter(Asset->AssetName.c_str()))
                            {
                                continue;
                            }

                            if (!AssetPickerFilter::PassesSourceFilter(Asset->Path))
                            {
                                continue;
                            }

                            ImGui::PushID(Asset);
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, 42.0f);
                            ImGui::TableSetColumnIndex(0);

                            const bool bSelected = ImGui::Selectable("##sel", false,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 42.0f));

                            ImGuiX::TextTooltip("{}", Asset->Path);
                            ImGui::SameLine();

                            if (FPackageThumbnail* Thumbnail = CThumbnailManager::Get().GetThumbnailForPackage(FName(Asset->Path.c_str())))
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
                                Path.SetPath(FStringView(Asset->Path.c_str(), Asset->Path.size()));
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

            ImGui::BeginDisabled(!bHasPath);
            if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", GSoftButtonSize))
            {
                ImGui::SetClipboardText(PathString.c_str());
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", GSoftButtonSize))
            {
                Path.Reset();
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

    void FSoftObjectPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(Path);
    }

    void FSoftObjectPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&Path);
    }
}
