#include "ReplaceReferencesModal.h"
#include "Core/CoreEditorDelegates.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Archive/ObjectReferenceReplacerArchive.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include <imgui.h>
#include "Containers/StringFormat.h"

namespace Lumina::ReplaceReferences
{
    namespace
    {
        constexpr ImVec4 kWarningColor = ImVec4(1.00f, 0.78f, 0.40f, 1.00f);
        constexpr ImVec4 kDangerColor  = ImVec4(0.96f, 0.36f, 0.38f, 1.00f);
        constexpr ImVec4 kDimColor     = ImVec4(0.55f, 0.56f, 0.62f, 1.00f);

        constexpr const char* kClearLabel = "Clear reference (null)";

        enum class EPhase : uint8
        {
            Choose,
            Applying,
            Done,
        };

        struct FModalState
        {
            EPhase                          Phase = EPhase::Choose;
            EReferenceFixupMode             Mode = EReferenceFixupMode::BeforeDelete;

            TVector<FAssetReferenceFixup>   Entries;
            THashSet<FGuid>                 ExcludedGUIDs;
            uint32                          TargetCount = 0;
            TFunction<void(bool)>           OnResolved;

            // Unique referencer GUIDs across every entry, drained a few per frame while applying.
            TVector<FGuid>                  Work;
            uint32                          WorkIndex = 0;

            // Referencers already resident with unsaved edits, which the fixup save commits along the way.
            uint32                          DirtyReferencerCount = 0;

            THashSet<CPackage*>             VisitedPackages;
            uint32                          Rewritten = 0;
            uint32                          Failed = 0;
            FName                           Current;

            TUniquePtr<FObjectReferenceReplacerArchive> Archive;
            TUniquePtr<FScopedAssetRegistryBatch>       RegistryBatch;

            ImGuiTextFilter                 PickerFilter;
        };

        const FAssetData* FindAsset(const FGuid& GUID)
        {
            return GUID.IsValid() ? FAssetRegistry::Get().GetAssetByGUID(GUID) : nullptr;
        }

        void DrawReplacementPicker(FAssetReferenceFixup& Entry, const THashSet<FGuid>& ExcludedGUIDs, ImGuiTextFilter& Filter)
        {
            const FAssetData* Selected = FindAsset(Entry.ReplacementGUID);
            const char* Preview = Selected != nullptr ? Selected->AssetName.c_str() : kClearLabel;

            ImGui::SetNextItemWidth(-1.0f);
            if (!ImGui::BeginCombo("##Replacement", Preview))
            {
                return;
            }

            if (ImGui::IsWindowAppearing())
            {
                Filter.Clear();
            }

            Filter.Draw("##Search", -1.0f);

            if (ImGui::Selectable(kClearLabel, !Entry.ReplacementGUID.IsValid()))
            {
                Entry.ReplacementGUID = FGuid();
            }

            ImGui::Separator();

            if (Entry.AssetClass == nullptr)
            {
                ImGui::TextColored(kDimColor, "Unknown asset type, no replacement can be offered.");
                ImGui::EndCombo();
                return;
            }

            TVector<FAssetData*> Candidates = FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
            {
                if (Data.AssetGUID == Entry.AssetGUID || ExcludedGUIDs.find(Data.AssetGUID) != ExcludedGUIDs.end())
                {
                    return false;
                }

                // Retargeting a referencer onto itself would mint a self-reference where a real edge was.
                for (const FGuid& ReferencerGUID : Entry.Referencers)
                {
                    if (ReferencerGUID == Data.AssetGUID)
                    {
                        return false;
                    }
                }

                CClass* DataClass = FindObject<CClass>(Data.AssetClass);
                return DataClass != nullptr && DataClass->IsChildOf(Entry.AssetClass);
            });

            for (const FAssetData* Candidate : Candidates)
            {
                if (!ImGuiX::PassSearchFilter(Filter, Candidate->AssetName.c_str()))
                {
                    continue;
                }

                ImGui::PushID(Candidate);
                if (ImGui::Selectable(Candidate->AssetName.c_str(), Candidate->AssetGUID == Entry.ReplacementGUID))
                {
                    Entry.ReplacementGUID = Candidate->AssetGUID;
                }
                ImGuiX::TextTooltip("{}", Candidate->Path);
                ImGui::PopID();
            }

            ImGui::EndCombo();
        }

        void DrawEntryTable(FModalState& State)
        {
            constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                                 | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

            if (!ImGui::BeginTable("##ReferencedAssets", 3, TableFlags, ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.4f)))
            {
                return;
            }

            const char* SubjectColumn = State.Mode == EReferenceFixupMode::BeforeDelete ? "Asset Being Deleted" : "Asset";
            ImGui::TableSetupColumn(SubjectColumn, ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableSetupColumn("Used By", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Replace With", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (SIZE_T Index = 0; Index < State.Entries.size(); ++Index)
            {
                FAssetReferenceFixup& Entry = State.Entries[Index];

                ImGui::PushID((int)Index);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                const bool bExpanded = ImGui::TreeNodeEx("##Entry", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", Entry.AssetName.c_str());
                ImGuiX::TextTooltip("{}", Entry.AssetPath);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", (uint32)Entry.Referencers.size());

                ImGui::TableSetColumnIndex(2);
                DrawReplacementPicker(Entry, State.ExcludedGUIDs, State.PickerFilter);

                if (bExpanded)
                {
                    for (const FGuid& ReferencerGUID : Entry.Referencers)
                    {
                        const FAssetData* Referencer = FindAsset(ReferencerGUID);

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Indent();
                        ImGui::TextColored(kDimColor, "%s", Referencer != nullptr ? Referencer->Path.c_str() : "<missing>");
                        ImGui::Unindent();

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextColored(kDimColor, "%s", Referencer != nullptr ? Referencer->AssetClass.c_str() : "");
                    }
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        void BeginApply(FModalState& State)
        {
            State.Archive = MakeUnique<FObjectReferenceReplacerArchive>();

            for (const FAssetReferenceFixup& Entry : State.Entries)
            {
                FFixedString ReplacementPath;
                FName        ReplacementName;
                if (const FAssetData* ReplacementData = FindAsset(Entry.ReplacementGUID))
                {
                    ReplacementPath = ReplacementData->Path;
                    ReplacementName = ReplacementData->AssetName;
                }

                CObject* Target = LoadObject<CObject>(Entry.AssetGUID);
                CObject* Replacement = Entry.ReplacementGUID.IsValid() ? LoadObject<CObject>(Entry.ReplacementGUID) : nullptr;

                if (Entry.ReplacementGUID.IsValid() && Replacement == nullptr)
                {
                    ImGuiX::Notifications::NotifyError("Could not load replacement '{0}'; those references are cleared instead.", ReplacementName);
                    ReplacementPath.clear();
                }

                if (Target != nullptr)
                {
                    State.Archive->AddReplacement(Target, Replacement);
                }
                else
                {
                    // Without the object itself, hard refs cannot be matched by pointer, only soft ones by GUID.
                    LOG_ERROR("Replace references: could not load '{}'; its hard references cannot be fixed up", Entry.AssetPath);
                    ++State.Failed;
                }

                const FGuid ReplacementGUID = Replacement != nullptr ? Entry.ReplacementGUID : FGuid();

                State.Archive->AddSoftReplacement(Entry.AssetGUID,
                    FStringView(Entry.AssetPath.c_str(), Entry.AssetPath.size()),
                    ReplacementGUID,
                    FStringView(ReplacementPath.c_str(), ReplacementPath.size()));
            }

            // One registry broadcast for the whole sweep instead of one per package saved.
            State.RegistryBatch = MakeUnique<FScopedAssetRegistryBatch>();
        }

        void TickApply(FModalState& State)
        {
            // Bounded per frame so the progress bar keeps moving; loading a world referencer is not cheap.
            constexpr uint32 kReferencersPerFrame = 2;

            uint32 Processed = 0;
            while (Processed < kReferencersPerFrame && State.WorkIndex < State.Work.size())
            {
                const FGuid ReferencerGUID = State.Work[State.WorkIndex];
                ++State.WorkIndex;
                ++Processed;

                CObject* Referencer = LoadObject<CObject>(ReferencerGUID);
                if (Referencer == nullptr)
                {
                    LOG_ERROR("Replace references: could not load referencing asset {}", ReferencerGUID.ToString());
                    ++State.Failed;
                    continue;
                }

                State.Current = Referencer->GetName();

                CPackage* Package = Referencer->GetPackage();
                if (Package == nullptr || Package->IsTransientPackage())
                {
                    ++State.Failed;
                    continue;
                }

                if (State.VisitedPackages.find(Package) != State.VisitedPackages.end())
                {
                    continue;
                }
                State.VisitedPackages.insert(Package);

                State.Archive->ResetNumReplaced();

                TVector<CObject*> Objects;
                GetObjectsWithPackage(Package, Objects);
                for (CObject* Object : Objects)
                {
                    if (Object == nullptr || Object == Package || Object->HasAnyFlag(OF_MarkedDestroy))
                    {
                        continue;
                    }

                    Object->Serialize(*State.Archive);
                }

                // The registry edge can predate an edit that already dropped the reference.
                if (State.Archive->GetNumReplaced() == 0)
                {
                    continue;
                }

                FCoreEditorDelegates::OnAssetPreSave.Broadcast(Referencer);

                if (CPackage::SavePackage(Package, Package->GetPackagePath()))
                {
                    FAssetRegistry::Get().AssetSaved(Referencer);
                    FCoreEditorDelegates::OnAssetSaved.Broadcast(Referencer);
                    ++State.Rewritten;
                }
                else
                {
                    LOG_ERROR("Replace references: failed to save package {}", Package->GetName());
                    ++State.Failed;
                }
            }

            if (State.WorkIndex >= State.Work.size())
            {
                State.RegistryBatch.reset();
                State.Archive.reset();
                State.Phase = EPhase::Done;
            }
        }

        bool DrawChoosePhase(FModalState& State)
        {
            uint32 ReferencerCount = 0;
            uint32 ReplacedCount = 0;
            for (const FAssetReferenceFixup& Entry : State.Entries)
            {
                ReferencerCount += (uint32)Entry.Referencers.size();
                ReplacedCount += Entry.ReplacementGUID.IsValid() ? 1u : 0u;
            }

            const bool bDeleting = State.Mode == EReferenceFixupMode::BeforeDelete;

            if (bDeleting)
            {
                ImGui::TextColored(kWarningColor, LE_ICON_ALERT " %u asset(s) you are deleting are still in use.",
                    (uint32)State.Entries.size());

                ImGui::Spacing();
                ImGui::TextWrapped(
                    "%u other asset(s) reference them. Pick a replacement for each, or leave it as \"%s\" to null "
                    "every reference to it. Referencing assets are loaded and saved to disk before anything is deleted.",
                    ReferencerCount, kClearLabel);
            }
            else
            {
                ImGui::TextColored(kWarningColor, LE_ICON_LINK " %u asset(s) reference the selection.", ReferencerCount);

                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Pick what those references should point at instead, or leave it as \"%s\" to null them. "
                    "Referencing assets are loaded and saved to disk. Nothing is deleted.",
                    kClearLabel);
            }

            ImGui::Spacing();
            ImGui::TextColored(kDimColor, "Replacements are limited to the original asset's own type or a subclass of it.");

            if (State.DirtyReferencerCount > 0)
            {
                ImGui::TextColored(kWarningColor, LE_ICON_ALERT " %u referencing asset(s) have unsaved changes, which this saves too.",
                    State.DirtyReferencerCount);
            }

            ImGui::Spacing();

            DrawEntryTable(State);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool bCanceled = ImGui::Button("Cancel", ImVec2(140.0f, 0.0f));

            ImGui::SameLine();

            FFixedString ConfirmLabel;
            if (bDeleting)
            {
                FormatTo(ConfirmLabel, LE_ICON_TRASH_CAN " Delete {} Item(s)", State.TargetCount);
                ImGui::PushStyleColor(ImGuiCol_Text, kDangerColor);
            }
            else
            {
                FormatTo(ConfirmLabel, LE_ICON_LINK " Update {} Reference(s)", ReferencerCount);
                ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
            }

            const bool bConfirmed = ImGui::Button(ConfirmLabel.c_str(), ImVec2(220.0f, 0.0f));
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::TextColored(kDimColor, "%u replace, %u clear", ReplacedCount, (uint32)State.Entries.size() - ReplacedCount);

            if (bCanceled)
            {
                if (State.OnResolved)
                {
                    State.OnResolved(false);
                }
                return true;
            }

            if (bConfirmed)
            {
                BeginApply(State);
                State.Phase = EPhase::Applying;
            }

            return false;
        }

        bool DrawApplyingPhase(FModalState& State)
        {
            TickApply(State);

            const float Progress = State.Work.empty()
                ? 1.0f
                : (float)((double)State.WorkIndex / (double)State.Work.size());

            ImGui::Text("Updating referencing assets... %u / %u", State.WorkIndex, (uint32)State.Work.size());
            ImGui::Spacing();
            ImGui::ProgressBar(Progress, ImVec2(-1.0f, 0.0f));
            ImGui::Spacing();
            ImGui::TextColored(kDimColor, "%s", State.Current.IsNone() ? "" : State.Current.c_str());

            return false;
        }

        bool DrawDonePhase(FModalState& State)
        {
            const bool bDeleting = State.Mode == EReferenceFixupMode::BeforeDelete;

            ImGui::Text("Updated and saved %u package(s).", State.Rewritten);

            if (State.Failed > 0)
            {
                ImGui::Spacing();
                ImGui::TextColored(kDangerColor, LE_ICON_ALERT " %u reference(s) could not be updated.", State.Failed);
                ImGui::TextWrapped(bDeleting
                    ? "Deleting now leaves them pointing at a missing asset. See the log for which ones failed."
                    : "Those references still point at the original asset. See the log for which ones failed.");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (!bDeleting)
            {
                if (ImGui::Button("Close", ImVec2(140.0f, 0.0f)))
                {
                    if (State.OnResolved)
                    {
                        State.OnResolved(false);
                    }
                    return true;
                }
                return false;
            }

            if (State.Failed > 0)
            {
                if (ImGui::Button("Cancel Delete", ImVec2(160.0f, 0.0f)))
                {
                    if (State.OnResolved)
                    {
                        State.OnResolved(false);
                    }
                    return true;
                }
                ImGui::SameLine();
            }

            const char* ProceedLabel = State.Failed > 0 ? LE_ICON_TRASH_CAN " Delete Anyway" : LE_ICON_TRASH_CAN " Delete";
            if (ImGui::Button(ProceedLabel, ImVec2(180.0f, 0.0f)))
            {
                if (State.OnResolved)
                {
                    State.OnResolved(true);
                }
                return true;
            }

            return false;
        }
    }

    TVector<FAssetReferenceFixup> BuildPlan(const TVector<FFixedString>& Paths)
    {
        FAssetRegistry& Registry = FAssetRegistry::Get();

        TVector<FFixedString> AssetPaths;
        for (const FFixedString& Path : Paths)
        {
            if (VFS::IsDirectory(Path))
            {
                VFS::RecursiveDirectoryIterator(Path, [&](const VFS::FFileInfo& FileInfo)
                {
                    if (FileInfo.IsDirectory())
                    {
                        return;
                    }

                    FFixedString Contained(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                    if (VFS::HasExtension(Contained, ".lasset"))
                    {
                        AssetPaths.push_back(Move(Contained));
                    }
                });
                continue;
            }

            if (VFS::HasExtension(Path, ".lasset"))
            {
                AssetPaths.push_back(Path);
            }
        }

        THashSet<FGuid> ExcludedGUIDs;
        TVector<FAssetReferenceFixup> Targets;
        for (const FFixedString& Path : AssetPaths)
        {
            const FAssetData* Data = Registry.GetAssetByPath(Path);
            if (Data == nullptr)
            {
                continue;
            }

            ExcludedGUIDs.insert(Data->AssetGUID);

            FAssetReferenceFixup Entry;
            Entry.AssetGUID  = Data->AssetGUID;
            Entry.AssetPath  = Data->Path;
            Entry.AssetName  = Data->AssetName;
            Entry.AssetClass = FindObject<CClass>(Data->AssetClass);
            Targets.push_back(Move(Entry));
        }

        TVector<FAssetReferenceFixup> Plan;
        for (FAssetReferenceFixup& Entry : Targets)
        {
            for (const FAssetData* Referencer : Registry.GetReferencersOf(Entry.AssetGUID))
            {
                // An asset in the subject set is never fixed up, so its own edges are left alone.
                if (Referencer == nullptr || ExcludedGUIDs.find(Referencer->AssetGUID) != ExcludedGUIDs.end())
                {
                    continue;
                }

                Entry.Referencers.push_back(Referencer->AssetGUID);
            }

            if (!Entry.Referencers.empty())
            {
                Plan.push_back(Move(Entry));
            }
        }

        return Plan;
    }

    void OpenModal(IEditorToolContext* Context, TVector<FAssetReferenceFixup> Plan, EReferenceFixupMode Mode,
                   uint32 TargetCount, TFunction<void(bool)> OnResolved)
    {
        if (Context == nullptr || Plan.empty())
        {
            if (OnResolved)
            {
                OnResolved(false);
            }
            return;
        }

        TSharedPtr<FModalState> State = MakeShared<FModalState>();
        State->Entries     = Move(Plan);
        State->Mode        = Mode;
        State->TargetCount = TargetCount;
        State->OnResolved  = Move(OnResolved);

        THashSet<FGuid> UniqueReferencers;
        for (const FAssetReferenceFixup& Entry : State->Entries)
        {
            State->ExcludedGUIDs.insert(Entry.AssetGUID);

            for (const FGuid& ReferencerGUID : Entry.Referencers)
            {
                if (!UniqueReferencers.insert(ReferencerGUID).second)
                {
                    continue;
                }

                State->Work.push_back(ReferencerGUID);

                // FindObject rather than LoadObject: only an already resident package can be dirty.
                const CObject* Resident = FindObject<CObject>(ReferencerGUID);
                const CPackage* Package = Resident != nullptr ? Resident->GetPackage() : nullptr;
                if (Package != nullptr && Package->IsDirty())
                {
                    ++State->DirtyReferencerCount;
                }
            }
        }

        const FString Title = Mode == EReferenceFixupMode::BeforeDelete ? "Delete Assets" : "Replace References";

        Context->PushModal(Title, ImVec2(900.0f, 560.0f), [State]() -> bool
        {
            switch (State->Phase)
            {
            case EPhase::Choose:   return DrawChoosePhase(*State);
            case EPhase::Applying: return DrawApplyingPhase(*State);
            case EPhase::Done:
            default:               return DrawDonePhase(*State);
            }
        });
    }
}
