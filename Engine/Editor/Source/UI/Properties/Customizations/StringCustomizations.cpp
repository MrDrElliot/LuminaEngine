#include "CoreTypeCustomization.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/NamePicker.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>

namespace Lumina
{
    namespace
    {
        // Grows the backing FString as the user types, since a shader body outgrows any fixed buffer.
        int StringResizeCallback(ImGuiInputTextCallbackData* Data)
        {
            if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                FString* Str = static_cast<FString*>(Data->UserData);
                Str->resize(Data->BufTextLen);
                Data->Buf = Str->data();
            }
            return 0;
        }

        // Null when the property asks for no picker, which leaves it a plain text field.
        const INamePickerSource* ResolvePickerSource(FProperty* Property, FStringView& OutKind)
        {
            const FString* Kind = Property->TryGetMetadata("Picker");
            if (Kind == nullptr)
            {
                return nullptr;
            }

            OutKind = FStringView(Kind->c_str(), Kind->size());
            return NamePicker::Find(FName(Kind->c_str()));
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

        if (!PickerKind.empty())
        {
            DrawUnknownPicker(PickerKind);
            return EPropertyChangeOp::None;
        }

        char Buffer[256];
        strncpy(Buffer, DisplayValue.c_str(), sizeof(Buffer));
        Buffer[sizeof(Buffer) - 1] = 0;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##Name", Buffer, sizeof(Buffer)))
        {
            DisplayValue = FName(Buffer);
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
                DisplayValue.empty() ? FName() : FName(DisplayValue.c_str()), &PickerState };

            const FNamePickerResult PickerResult = NamePicker::Draw(*Source, PickerArgs);
            if (PickerResult.bChanged)
            {
                DisplayValue = PickerResult.Value.IsNone() ? FString() : FString(PickerResult.Value.c_str());
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

        if (!PickerKind.empty())
        {
            DrawUnknownPicker(PickerKind);
            return EPropertyChangeOp::None;
        }

        // "FilePath" meta turns the field into an asset-path picker ("..." button, searchable).
        const bool bFilePath = Property->Property->HasMetadata("FilePath");
        // "Multiline" meta turns the field into a wrapping multi-line box (newlines allowed).
        const bool bMultiline = Property->Property->HasMetadata("Multiline");
        const float ButtonWidth = bFilePath ? ImGui::GetFrameHeight() : 0.0f;

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        char Buffer[1024];
        strncpy(Buffer, DisplayValue.c_str(), sizeof(Buffer));
        Buffer[sizeof(Buffer) - 1] = '\0';

        if (bMultiline)
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f);
            // Edited in place through the resize callback, since the text can be arbitrarily long.
            ImGui::InputTextMultiline("##ParamName", DisplayValue.data(), DisplayValue.capacity() + 1, Size,
                                      ImGuiInputTextFlags_CallbackResize, StringResizeCallback, &DisplayValue);
        }
        else
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
            if (ImGui::InputText("##ParamName", Buffer, sizeof(Buffer)))
            {
                DisplayValue = Buffer;
            }
            ImGui::PopItemWidth();
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
                    TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; });
                    for (const FAssetData* Asset : Assets)
                    {
                        if (!ImGuiX::PassSearchFilter(SearchFilter, Asset->Path.c_str()))
                        {
                            continue;
                        }

                        if (ImGui::Selectable(Asset->Path.c_str()))
                        {
                            DisplayValue = Asset->Path.c_str();
                            Result = EPropertyChangeOp::Updated;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
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
