#include "UIDocumentPropertyCustomization.h"

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
        // The object picker's sizes, so a document slot and an asset slot read as the same control.
        constexpr ImVec2 GButtonSize(42, 0);
        constexpr ImVec2 GTileSize(64, 64);

        // "/Game/UI/Menu.rml" reads as "Menu" in "/Game/UI".
        void SplitDocumentPath(FStringView Path, FStringView& OutName, FStringView& OutFolder)
        {
            const size_t Slash = Path.find_last_of('/');
            OutFolder = Slash == FStringView::npos ? FStringView() : Path.substr(0, Slash);
            OutName   = Slash == FStringView::npos ? Path : Path.substr(Slash + 1);

            const size_t Dot = OutName.find_last_of('.');
            if (Dot != FStringView::npos)
            {
                OutName = OutName.substr(0, Dot);
            }
        }

        bool IsDocumentPath(FStringView Path)
        {
            return TextAsset::KindFromPath(Path) == ETextAssetKind::RmlDocument;
        }

        void OpenInUIEditor(FStringView Path)
        {
            if (IEditorToolContext* Tools = SessionOps::GetToolContext(); Tools != nullptr && !Path.empty())
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
        FUIDocumentRef* Ref = static_cast<FUIDocumentRef*>(Property->GetValuePtr());
        if (Ref == nullptr)
        {
            return EPropertyChangeOp::None;
        }

        const FStringView ResolvedView = Ref->ResolvePath();
        const FString     Resolved(ResolvedView.data(), ResolvedView.size());
        const bool        bHasDocument = !Resolved.empty();
        const bool        bMissing     = bHasDocument && FAssetRegistry::Get().GetTextAssetByPath(ResolvedView) == nullptr;

        FStringView Name, Folder;
        SplitDocumentPath(ResolvedView, Name, Folder);

        bool bWasChanged = false;
        auto Assign = [&](TFunction<void()> Mutation)
        {
            PendingMutation = Move(Mutation);
            bWasChanged = true;
        };

        FEditorUI* EditorUI = static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushID(this);
        if (ImGui::BeginChild("UD", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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
                    const FString NewPath(Dropped.c_str(), Dropped.size());
                    Assign([Ref, NewPath] { Ref->SetPath(NewPath); });
                }
                ImGui::EndDragDropTarget();
            }
            ImGuiX::TextTooltip("{}", bHasDocument ? "Double-click to open in the UI editor, or drop an .rml here." : "Drop an .rml here.");

            ImGui::SameLine();
            ImGui::BeginGroup();

            const FString Preview = !bHasDocument ? FString("<None>")
                                  : bMissing      ? FString(LE_ICON_ALERT " ") + FString(Name.data(), Name.size())
                                  :                 FString(Name.data(), Name.size());
            const ImVec4 PreviewColor = bHasDocument && !bMissing ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 0.19f, 0.19f, 1.0f);

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::PushStyleColor(ImGuiCol_Text, PreviewColor);
            const bool bOpen = ImGui::BeginCombo("##Pick", Preview.c_str(), ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft);
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
                }
                SearchFilter.Draw("##Search", -1.0f);

                if (ImGui::Selectable(LE_ICON_CLOSE " None", !bHasDocument))
                {
                    Assign([Ref] { Ref->Reset(); });
                }
                ImGui::Separator();

                TVector<FTextAssetData*> Documents = FAssetRegistry::Get().GetTextAssetsOfKind(ETextAssetKind::RmlDocument);
                Algo::Sort(Documents, [](const FTextAssetData* A, const FTextAssetData* B) { return strcmp(A->Path.c_str(), B->Path.c_str()) < 0; });

                for (const FTextAssetData* Data : Documents)
                {
                    if (!ImGuiX::PassSearchFilter(SearchFilter, Data->Path.c_str()))
                    {
                        continue;
                    }

                    FStringView ItemName, ItemFolder;
                    SplitDocumentPath(FStringView(Data->Path.c_str(), Data->Path.size()), ItemName, ItemFolder);

                    ImGui::PushID(Data);
                    const bool bCurrent = FStringView(Data->Path.c_str(), Data->Path.size()) == ResolvedView;
                    const FString Label = FString(LE_ICON_VIEW_DASHBOARD_OUTLINE " ") + FString(ItemName.data(), ItemName.size());
                    if (ImGui::Selectable(Label.c_str(), bCurrent))
                    {
                        const FString NewPath(Data->Path.c_str(), Data->Path.size());
                        const FGuid   NewGuid = Data->Guid;
                        Assign([Ref, NewPath, NewGuid] { Ref->Set(NewPath, NewGuid); });
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.*s", (int)ItemFolder.size(), ItemFolder.data());
                    ImGui::PopID();
                }

                if (Documents.empty())
                {
                    ImGui::TextDisabled("No .rml documents in the project.");
                }

                ImGui::EndCombo();
            }

            // Outside the disabled block, since this is most useful exactly when the slot is empty.
            const FFixedString Selected = EditorUI != nullptr ? EditorUI->GetContentBrowserSelectedFilePath() : FFixedString();
            const bool bSelectionFits = IsDocumentPath(FStringView(Selected.c_str(), Selected.size()));

            ImGui::BeginDisabled(!bSelectionFits);
            if (ImGui::Button(LE_ICON_ARROW_LEFT_CIRCLE "##UseSelected", GButtonSize))
            {
                const FString NewPath(Selected.c_str(), Selected.size());
                Assign([Ref, NewPath] { Ref->SetPath(NewPath); });
            }
            ImGui::EndDisabled();
            ImGuiX::TextTooltip("{}", bSelectionFits
                ? "Use the document selected in the Content Browser."
                : "Select an .rml in the Content Browser to use it here.");

            ImGui::SameLine();

            ImGui::BeginDisabled(!bHasDocument);

            if (ImGui::Button(LE_ICON_MAGNIFY "##Browse", GButtonSize) && EditorUI != nullptr)
            {
                EditorUI->BrowseToAsset(ResolvedView);
            }
            ImGuiX::TextTooltip("{}", "Show this document in the Content Browser.");

            ImGui::SameLine();

            if (ImGui::Button(LE_ICON_CONTENT_COPY "##Copy", GButtonSize))
            {
                ImGui::SetClipboardText(Resolved.c_str());
            }
            ImGuiX::TextTooltip("{}", "Copy the path.");

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button(LE_ICON_CLOSE_CIRCLE "##Clear", GButtonSize))
            {
                Assign([Ref] { Ref->Reset(); });
            }
            ImGui::PopStyleColor(3);
            ImGuiX::TextTooltip("{}", "Clear the document.");

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

    void FUIDocumentPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = nullptr;
        }
    }
}
