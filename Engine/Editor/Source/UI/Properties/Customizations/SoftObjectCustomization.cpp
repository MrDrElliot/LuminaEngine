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
#include "Containers/Vector.h"
#include <Containers/String.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Core/Object/Package/Package.h>
#include <imgui_internal.h>
#include "Thumbnails/ThumbnailManager.h"
#include "UI/EditorUI.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"
#include <LuminaEditor.h>

namespace Lumina
{
    // Trailing path segment for a compact label ("/Game/Maps/Main" -> "Main").
    static FFixedString SoftLeafName(FStringView PathView)
    {
        if (PathView.IsEmpty())
        {
            return FFixedString("<None>");
        }
        int32 Slash = PathView.FindLastOf('/');
        FStringView Leaf = (Slash == Constants::kIndexNone) ? PathView : PathView.Mid(Slash + 1);
        return FFixedString(Leaf.GetData(), Leaf.Len());
    }

    EPropertyChangeOp FSoftObjectPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FSoftObjectProperty* SoftProperty = static_cast<FSoftObjectProperty*>(Property->Property);
        CClass* PropertyClass = SoftProperty->GetPropertyClass();

        bool bWasChanged = false;

        const FStringView PathView = Path.GetPath();
        const bool bHasPath = !PathView.IsEmpty();
        const FName PathName = bHasPath ? FName(FString(PathView.GetData(), PathView.Len())) : NAME_None;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushID(this);
        // A group rather than an auto-sizing child, which keeps a stale height while clipped and collapses the row under it.
        ImGui::BeginGroup();
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
            if (!ButtonTexture.IsSet())
            {
                ButtonTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png");
            }

            const FFixedString Leaf = SoftLeafName(PathView);
            ImGui::BeginDisabled(!bHasPath);
            ImGui::ImageButton(*Leaf, ButtonTexture.GetValue(), ImVec2(64, 64));
            ImGui::EndDisabled();

            // Binds to a dragged asset of the property's class by path, with no load.
            if (ImGui::BeginDragDropTarget())
            {
                if (CObject* Dropped = DragDrop::AcceptAssetOfClass(PropertyClass))
                {
                    if (Dropped->GetPackage())
                    {
                        const FString DroppedPath = Dropped->GetPackage()->GetName().ToString();
                        Path.SetPath(FStringView(*DroppedPath, DroppedPath.Len()));
                        bWasChanged = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Opening loads the asset, which nothing else on this row does.
            if (bHasPath && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (const FAssetDataPtr Asset = FAssetRegistry::Get().GetAssetByPath(PathView))
                {
                    static_cast<FEditorUI*>(Global::GEditorEngine->GetDevelopmentToolsUI())->OpenAssetEditor(Asset->AssetGUID);
                }
            }

            ImGui::SameLine();
            ImGui::BeginGroup();

            const float ComboArrowWidth = ImGui::GetFrameHeight();
            const float TotalPathWidgetWidth = ImGui::GetContentRegionAvail().x;
            const float TextWidgetWidth = TotalPathWidgetWidth - ComboArrowWidth;

            ImGui::SetNextItemWidth(TextWidgetWidth);

            FFixedString PathString = bHasPath ? FFixedString(PathView.GetData(), PathView.Len()) : FFixedString("<None>");
            ImGui::PushStyleColor(ImGuiCol_Text, bHasPath ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 0.19f, 0.19f, 1.0f));
            ImGuiX::InputText("##SoftObjectPathText", PathString, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
            ImGuiX::TextTooltip("{}", PathString);
            ImGui::PopStyleColor();

            const ImVec2 ComboDropDownSize = ImMax(ImVec2(200, 200), ImVec2(TextWidgetWidth, 300.0f));

            ImGui::SameLine(0, 0);
            if (ImGui::BeginCombo("##SoftObjectPath", "", ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview))
            {
                SearchFilter.Draw("##Search", ComboDropDownSize.x - 30.0f);
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere(-1);
                }
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
                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 4));
                    if (ImGui::BeginTable("##AssetTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner))
                    {
                        ImGui::TableSetupColumn("##Thumb", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                        ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                        for (const FAssetDataPtr& Asset : AssetsOfClass.Get(PropertyClass))
                        {
                            if (!ImGuiX::PassSearchFilter(SearchFilter, *Asset->AssetName))
                            {
                                continue;
                            }

                            if (!AssetPickerFilter::PassesSourceFilter(Asset->Path))
                            {
                                continue;
                            }

                            ImGui::PushID(Asset.Get());
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, 42.0f);
                            ImGui::TableSetColumnIndex(0);

                            const bool bSelected = ImGui::Selectable("##sel", false,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 42.0f));

                            ImGuiX::TextTooltip("{}", Asset->Path);
                            ImGui::SameLine();

                            if (FPackageThumbnail* Thumbnail = CThumbnailManager::Get().GetThumbnailForPackage(FName(*Asset->Path)))
                            {
                                ImGui::Image(ImGuiX::ToImTextureRef(Thumbnail->LoadedImage), ImVec2(42, 42));
                            }
                            else
                            {
                                ImGui::Image(ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png"), ImVec2(42, 42));
                            }

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(*Asset->AssetName);

                            if (bSelected)
                            {
                                Path.SetPath(FStringView(*Asset->Path, Asset->Path.Len()));
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

            // Binds by path like the list above, so taking the selection never loads the asset.
            const FAssetDataPtr BrowserSelection = AssetPickerFilter::GetSelectedAssetOfClass(PropertyClass);
            if (AssetPickerFilter::DrawUseSelectedButton(BrowserSelection != nullptr, "Select a compatible asset in the Content Browser to use it here."))
            {
                Path.SetPath(FStringView(*BrowserSelection->Path, BrowserSelection->Path.Len()));
                bWasChanged = true;
            }
            ImGui::SameLine();

            AssetPickerFilter::DrawBrowseButton(PathView);
            ImGui::SameLine();

            ImGui::BeginDisabled(!bHasPath);
            if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", AssetPickerFilter::kButtonSize))
            {
                ImGui::SetClipboardText(*PathString);
            }
            ImGui::SameLine();

            if (AssetPickerFilter::DrawClearButton("Clear the reference."))
            {
                Path.Reset();
                bWasChanged = true;
            }
            ImGui::EndDisabled();

            ImGui::EndGroup();
        }
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

    void FSoftObjectPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(Path);
    }

    void FSoftObjectPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->GetValue(&Path);
    }
}
