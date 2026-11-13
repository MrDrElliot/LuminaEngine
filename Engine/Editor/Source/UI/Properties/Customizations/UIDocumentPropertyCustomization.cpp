#include "UIDocumentPropertyCustomization.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"

#include "LuminaEditor.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "Session/SessionOps.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/EditorUI.h"
#include "UI/Tools/EditorToolContext.h"
#include "UI/UIDocumentRef.h"

namespace Lumina
{
    namespace
    {
        // The object picker's tile size, so a document slot and an asset slot read as the same control.
        constexpr ImVec2 GTileSize(64, 64);

        // "/Game/UI/Menu.rml" reads as "Menu" in "/Game/UI".
        void SplitDocumentPath(FStringView Path, FStringView& OutName, FStringView& OutFolder)
        {
            const int32 Slash = Path.FindLastOf('/');
            OutFolder = Slash == Constants::kIndexNone ? FStringView() : Path.Mid(0, Slash);
            OutName   = Slash == Constants::kIndexNone ? Path : Path.Mid(Slash + 1);

            const int32 Dot = OutName.FindLastOf('.');
            if (Dot != Constants::kIndexNone)
            {
                OutName = OutName.Mid(0, Dot);
            }
        }

        bool IsDocumentPath(FStringView Path)
        {
            return TextAsset::KindFromPath(Path) == ETextAssetKind::RmlDocument;
        }

        void OpenInUIEditor(FStringView Path)
        {
            if (IEditorToolContext* Tools = SessionOps::GetToolContext(); Tools != nullptr && !Path.IsEmpty())
            {
                Tools->OpenFileEditor(Path);
            }
        }
    }

    TSharedPtr<FUIDocumentPropertyCustomization> FUIDocumentPropertyCustomization::MakeInstance()
    {
        return MakeShared<FUIDocumentPropertyCustomization>();
    }

    EPropertyChangeOp FUIDocumentPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        SUIDocumentRef* Ref = static_cast<SUIDocumentRef*>(Property->GetValuePtr());
        if (Ref == nullptr)
        {
            return EPropertyChangeOp::None;
        }

        const FStringView ResolvedView = Ref->ResolvePath();
        const FString     Resolved(ResolvedView.GetData(), ResolvedView.Len());
        const bool        bHasDocument = !Resolved.IsEmpty();
        const bool        bMissing     = bHasDocument && FAssetRegistry::Get().GetTextAssetByPath(ResolvedView) == nullptr;

        FStringView Name, Folder;
        SplitDocumentPath(ResolvedView, Name, Folder);

        bool bWasChanged = false;
        auto Assign = [&](TFunction<void()> Mutation)
        {
            PendingMutation = Move(Mutation);
            bWasChanged = true;
        };

        FEditorUI* EditorUI = static_cast<FEditorUI*>(Global::GEditorEngine->GetDevelopmentToolsUI());

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushID(this);
        // A group rather than an auto-sizing child, which keeps a stale height while clipped and collapses the row under it.
        ImGui::BeginGroup();
        {
            // The tile stands where an asset's thumbnail does, as the drop target and the way into the document.
            ImGui::BeginDisabled(!bHasDocument);
            ImGui::Button(LE_ICON_VIEW_DASHBOARD_OUTLINE "##Doc", GTileSize);
            ImGui::EndDisabled();
            if (bHasDocument && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                OpenInUIEditor(ResolvedView);
            }
            if (ImGui::BeginDragDropTarget())
            {
                FFixedString Dropped;
                if (DragDrop::AcceptFile("rml", Dropped))
                {
                    const FString NewPath(*Dropped, Dropped.Len());
                    Assign([Ref, NewPath] { Ref->SetPath(NewPath); });
                }
                ImGui::EndDragDropTarget();
            }
            ImGuiX::TextTooltip("{}", bHasDocument ? "Double-click to open in the UI editor, or drop an .rml here." : "Drop an .rml here.");

            ImGui::SameLine();
            ImGui::BeginGroup();

            const FString Preview = !bHasDocument ? FString("<None>")
                                  : bMissing      ? FString(LE_ICON_ALERT " ") + FString(Name.GetData(), Name.Len())
                                  :                 FString(Name.GetData(), Name.Len());
            const ImVec4 PreviewColor = bHasDocument && !bMissing ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 0.19f, 0.19f, 1.0f);

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::PushStyleColor(ImGuiCol_Text, PreviewColor);
            const bool bOpen = ImGui::BeginCombo("##Pick", *Preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft);
            ImGui::PopStyleColor();
            if (!bOpen)
            {
                ImGuiX::TextTooltip("{}", bMissing ? FString("Missing ") + Resolved : bHasDocument ? Resolved : FString("No document"));
            }

            if (bOpen)
            {
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();

                    // Rebuilt once on popup open rather than every frame it stays open.
                    CachedDocuments = FAssetRegistry::Get().GetTextAssetsOfKind(ETextAssetKind::RmlDocument);
                    Algo::Sort(CachedDocuments, [](const FTextAssetDataPtr& A, const FTextAssetDataPtr& B) { return strcmp(*A->Path, *B->Path) < 0; });
                }
                SearchFilter.Draw("##Search", -1.0f);

                if (ImGui::Selectable(LE_ICON_CLOSE " None", !bHasDocument))
                {
                    Assign([Ref] { Ref->Reset(); });
                }
                ImGui::Separator();

                for (const FTextAssetDataPtr& Data : CachedDocuments)
                {
                    if (!ImGuiX::PassSearchFilter(SearchFilter, *Data->Path))
                    {
                        continue;
                    }

                    FStringView ItemName, ItemFolder;
                    SplitDocumentPath(FStringView(*Data->Path, Data->Path.Len()), ItemName, ItemFolder);

                    ImGui::PushID(Data.Get());
                    const bool bCurrent = FStringView(*Data->Path, Data->Path.Len()) == ResolvedView;
                    const FString Label = FString(LE_ICON_VIEW_DASHBOARD_OUTLINE " ") + FString(ItemName.GetData(), ItemName.Len());
                    if (ImGui::Selectable(*Label, bCurrent))
                    {
                        const FString NewPath(*Data->Path, Data->Path.Len());
                        const FGuid   NewGuid = Data->Guid;
                        Assign([Ref, NewPath, NewGuid] { Ref->Set(NewPath, NewGuid); });
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.*s", ItemFolder.Len(), ItemFolder.GetData());
                    ImGui::PopID();
                }

                if (CachedDocuments.IsEmpty())
                {
                    ImGui::TextDisabled("No .rml documents in the project.");
                }

                ImGui::EndCombo();
            }

            // Outside the disabled block, since this is most useful exactly when the slot is empty.
            const FFixedString Selected = EditorUI != nullptr ? EditorUI->GetContentBrowserSelectedFilePath() : FFixedString();
            const bool bSelectionFits = IsDocumentPath(FStringView(*Selected, Selected.Len()));

            if (AssetPickerFilter::DrawUseSelectedButton(bSelectionFits, "Select an .rml in the Content Browser to use it here."))
            {
                const FString NewPath(*Selected, Selected.Len());
                Assign([Ref, NewPath] { Ref->SetPath(NewPath); });
            }

            ImGui::SameLine();

            AssetPickerFilter::DrawBrowseButton(ResolvedView);

            ImGui::SameLine();

            ImGui::BeginDisabled(!bHasDocument);

            if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", AssetPickerFilter::kButtonSize))
            {
                ImGui::SetClipboardText(*Resolved);
            }
            ImGuiX::TextTooltip("{}", "Copy the path.");

            ImGui::SameLine();

            if (AssetPickerFilter::DrawClearButton("Clear the document."))
            {
                Assign([Ref] { Ref->Reset(); });
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

    void FUIDocumentPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = nullptr;
        }
    }
}
