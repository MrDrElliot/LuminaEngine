#include "CoreTypeCustomization.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/NamePicker.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "Session/SessionOps.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "UI/Tools/EditorToolContext.h"

namespace Lumina
{
    namespace
    {
        // Null when the property asks for no picker, which leaves it a plain text field.
        const INamePickerSource* ResolvePickerSource(FProperty* Property, FStringView& OutKind)
        {
            const FCStringView Kind = Property->GetMetadata("Picker");
            if (Kind.IsEmpty())
            {
                return nullptr;
            }

            OutKind = Kind;
            return NamePicker::Find(FName(*Kind));
        }

        void DrawUnknownPicker(FStringView Kind)
        {
            ImGui::TextUnformatted(LE_ICON_EXCLAMATION " Unknown picker");
            ImGuiX::TextTooltip("No picker named \"{}\" is registered", Kind);
        }
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FStringView PickerKind;
        if (const INamePickerSource* Source = ResolvePickerSource(Property->Property, PickerKind))
        {
            const FNamePickerArgs PickerArgs{ "##Pick", Args.Context, DisplayValue, &PickerState };
            const FNamePickerResult Result = NamePicker::Draw(*Source, PickerArgs);
            if (Result.bChanged)
            {
                DisplayValue = Result.Value;
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

        if (!PickerKind.IsEmpty())
        {
            DrawUnknownPicker(PickerKind);
            return EPropertyChangeOp::None;
        }

        // None edits as an empty field, which also turns a cleared field back into None.
        FString Text = DisplayValue.IsNone() ? FString() : DisplayValue.ToString();

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputTextWithHint("##Name", "None", Text.GetData(), Text.GetCapacity() + 1,
                                     ImGuiInputTextFlags_CallbackResize, ImGuiX::StringInputResizeCallback<FString>, &Text))
        {
            DisplayValue = FName(Text);
        }
        ImGui::PopItemWidth();

        return ImGui::IsItemDeactivatedAfterEdit() ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FNamePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FNamePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FName ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }

    EPropertyChangeOp FStringPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        // The string form of the picker, which is what a C# property field mints.
        FStringView PickerKind;
        if (const INamePickerSource* Source = ResolvePickerSource(Property->Property, PickerKind))
        {
            const FNamePickerArgs PickerArgs{ "##Pick", Args.Context,
                DisplayValue.IsEmpty() ? FName() : FName(*DisplayValue), &PickerState };

            const FNamePickerResult PickerResult = NamePicker::Draw(*Source, PickerArgs);
            if (PickerResult.bChanged)
            {
                DisplayValue = PickerResult.Value.IsNone() ? FString() : FString(*PickerResult.Value);
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

        if (!PickerKind.IsEmpty())
        {
            DrawUnknownPicker(PickerKind);
            return EPropertyChangeOp::None;
        }

        // "FilePath" meta turns the field into an asset-path picker ("..." button, searchable), and "AssetType" narrows it to those text files.
        const bool bTextAsset = Property->Property->HasMetadata("AssetType");
        const bool bFilePath = bTextAsset || Property->Property->HasEditorFlag(EPropertyEditorFlags::FilePath);
        const TVector<ETextAssetKind> TextKinds = bTextAsset ? TextAsset::ParseAssetTypeMeta(Property->Property->GetMetadata("AssetType")) : TVector<ETextAssetKind>();
        // "Multiline" meta turns the field into a wrapping multi-line box (newlines allowed).
        const bool bMultiline = Property->Property->HasEditorFlag(EPropertyEditorFlags::Multiline);
        const float ButtonWidth = bFilePath ? ImGui::GetFrameHeight() * (bTextAsset ? 2.0f : 1.0f) : 0.0f;

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        if (bMultiline)
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f);
            ImGui::InputTextMultiline("##ParamName", DisplayValue.GetData(), DisplayValue.GetCapacity() + 1, Size,
                                      ImGuiInputTextFlags_CallbackResize, ImGuiX::StringInputResizeCallback<FString>, &DisplayValue);
        }
        else
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
            ImGuiX::InputText("##ParamName", DisplayValue);
            ImGui::PopItemWidth();

            // A file of an allowed kind dragged from the content browser lands as its path.
            if (bTextAsset && ImGui::BeginDragDropTarget())
            {
                for (ETextAssetKind Kind : TextKinds)
                {
                    FStringView Ext = TextAsset::ExtensionForKind(Kind);
                    if (!Ext.IsEmpty() && Ext[0] == '.')
                    {
                        Ext = Ext.Mid(1);
                    }
                    FFixedString Dropped;
                    if (DragDrop::AcceptFile(Ext, Dropped))
                    {
                        DisplayValue = FString(*Dropped, Dropped.Len());
                        Result = EPropertyChangeOp::Updated;
                        break;
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Updated;
        }

        if (bFilePath)
        {
            ImGui::SameLine(0, 0);
            if (ImGui::Button(LE_ICON_DOTS_HORIZONTAL "##FilePathPick", ImVec2(ButtonWidth, 0)))
            {
                ImGui::OpenPopup("##FilePathPicker");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal("Pick asset path");
            }

            if (ImGui::BeginPopup("##FilePathPicker"))
            {
                SearchFilter.Draw("##Search", 250.0f);
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere(-1);
                }
                if (ImGui::BeginChild("##PathList", ImVec2(300, 300)))
                {
                    auto Offer = [&](const FString& Path)
                    {
                        if (!ImGuiX::PassSearchFilter(SearchFilter, *Path))
                        {
                            return;
                        }
                        if (ImGui::Selectable(*Path))
                        {
                            DisplayValue = Path;
                            Result = EPropertyChangeOp::Updated;
                            ImGui::CloseCurrentPopup();
                        }
                    };

                    if (bTextAsset)
                    {
                        for (ETextAssetKind Kind : TextKinds)
                        {
                            for (const FTextAssetData* Data : FAssetRegistry::Get().GetTextAssetsOfKind(Kind))
                            {
                                Offer(FString(*Data->Path, Data->Path.Len()));
                            }
                        }
                    }
                    else
                    {
                        for (const FAssetData* Asset : FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; }))
                        {
                            Offer(FString(*Asset->Path));
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }

            // A text asset opens in the engine's own editor for it, such as the UI editor for an .rml.
            if (bTextAsset)
            {
                ImGui::SameLine(0, 0);
                ImGui::BeginDisabled(DisplayValue.IsEmpty());
                if (ImGui::Button(LE_ICON_OPEN_IN_NEW "##OpenTextAsset", ImVec2(ImGui::GetFrameHeight(), 0)))
                {
                    if (IEditorToolContext* Tools = SessionOps::GetToolContext())
                    {
                        Tools->OpenFileEditor(FStringView(*DisplayValue, DisplayValue.Len()));
                    }
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGuiX::TextTooltip_Internal("Open in its editor");
                }
            }
        }

        return Result;
    }

    
    void FStringPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(DisplayValue);
    }

    void FStringPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FString ActualValue;
        Property->GetValue(&ActualValue);

        // An unconditional copy would throw away an in-progress edit, which commits only on deactivate.
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
