#include "BulkRename.h"

#include "EditorToolContext.h"
#include "Asset/AssetOps.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    static constexpr const char* kInvalidNameCharacters = "<>:\"/\\|?*";
    static constexpr int32       kMaxNumberDigits = 8;

    namespace
    {
        FString ReplaceAll(FStringView Name, FStringView Find, FStringView Replace, bool bMatchCase)
        {
            if (Find.empty())
            {
                return FString(Name);
            }

            // Lowering both keeps every offset, since only ASCII letters change case.
            FString Haystack(Name);
            FString Needle(Find);
            if (!bMatchCase)
            {
                Haystack.ToLower();
                Needle.ToLower();
            }

            FString Out;
            size_t Position = 0;
            for (size_t Hit = Haystack.find(Needle, Position); Hit != FString::npos; Hit = Haystack.find(Needle, Position))
            {
                Out.append(Name.substr(Position, Hit - Position));
                Out.append(Replace);
                Position = Hit + Needle.size();
            }
            Out.append(Name.substr(Position));
            return Out;
        }

        FStringView EntryName(const FBulkRenameEntry& Entry)
        {
            return VFS::FileName(FStringView(Entry.SourcePath.c_str(), Entry.SourcePath.size()), !Entry.bDirectory);
        }

        const char* DescribeStatus(EBulkRenameStatus Status)
        {
            switch (Status)
            {
            case EBulkRenameStatus::Ready:             return "Ready";
            case EBulkRenameStatus::Unchanged:         return "Unchanged";
            case EBulkRenameStatus::Empty:             return "Empty name";
            case EBulkRenameStatus::InvalidCharacters: return "Invalid characters";
            case EBulkRenameStatus::Duplicate:         return "Same as another new name";
            case EBulkRenameStatus::Exists:            return "Name already taken";
            }
            return "";
        }

        ImVec4 StatusColor(EBulkRenameStatus Status)
        {
            switch (Status)
            {
            case EBulkRenameStatus::Ready:     return EditorColors::Success();
            case EBulkRenameStatus::Unchanged: return EditorColors::TextDim();
            default:                           return EditorColors::Danger();
            }
        }

        bool DrawTextField(const char* Label, FString& Value)
        {
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Label);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID(Label);
            const bool bChanged = ImGuiX::InputText("##Field", Value);
            ImGui::PopID();
            return bChanged;
        }

        bool DrawRuleInputs(FBulkRenameRule& Rule)
        {
            bool bChanged = false;
            const float Scale = ImGuiX::GetUIScale();

            if (ImGui::BeginTable("##Rule", 4, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("##FindLabel", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##Find", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##ReplaceLabel", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##Replace", ImGuiTableColumnFlags_WidthStretch);
                bChanged |= DrawTextField("Find", Rule.Find);
                bChanged |= DrawTextField("Replace", Rule.Replace);
                bChanged |= DrawTextField("Prefix", Rule.Prefix);
                bChanged |= DrawTextField("Suffix", Rule.Suffix);
                ImGui::EndTable();
            }

            bChanged |= ImGui::Checkbox("Match Case", &Rule.bMatchCase);
            ImGui::SameLine(0.0f, 24.0f * Scale);
            bChanged |= ImGui::Checkbox("Number", &Rule.bNumber);
            ImGuiX::TextTooltip("Ends each name with _ and its place in the list below, counted from Start.");
            ImGui::BeginDisabled(!Rule.bNumber);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f * Scale);
            bChanged |= ImGui::InputInt("Start", &Rule.NumberStart);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f * Scale);
            bChanged |= ImGui::SliderInt("Digits", &Rule.NumberDigits, 1, kMaxNumberDigits);
            ImGui::EndDisabled();

            return bChanged;
        }

        void DrawPreview(const TVector<FBulkRenameEntry>& Entries)
        {
            constexpr ImGuiTableFlags Flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
            const float FooterHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
            if (!ImGui::BeginTable("##Preview", 3, Flags, ImVec2(0.0f, -FooterHeight)))
            {
                return;
            }

            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("New");
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 170.0f * ImGuiX::GetUIScale());
            ImGui::TableHeadersRow();

            ImGuiListClipper Clipper;
            Clipper.Begin((int32)Entries.size());
            while (Clipper.Step())
            {
                for (int32 Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
                {
                    const FBulkRenameEntry& Entry = Entries[Index];
                    const FStringView Current = EntryName(Entry);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(Current.data(), Current.data() + Current.size());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(Entry.NewName.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(StatusColor(Entry.Status), "%s", DescribeStatus(Entry.Status));
                }
            }

            ImGui::EndTable();
        }

        void RunBulkRename(const TVector<FBulkRenameEntry>& Entries, const TFunction<void()>& OnRenamed)
        {
            int32 Renamed = 0;
            for (const FBulkRenameEntry& Entry : Entries)
            {
                const AssetOps::FPathOpResult Result = AssetOps::MovePath(Entry.SourcePath, Entry.TargetPath);
                if (Result.bSucceeded)
                {
                    ++Renamed;
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Could not rename {0}. {1}", Entry.SourcePath, Result.Error);
                }
            }

            if (Renamed > 0)
            {
                ImGuiX::Notifications::NotifySuccess("Renamed {0} item(s).", Renamed);
                if (OnRenamed)
                {
                    OnRenamed();
                }
            }
        }
    }

    FString ApplyBulkRenameRule(const FBulkRenameRule& Rule, FStringView Name, int32 Index)
    {
        FString Out = Rule.Prefix;
        Out.append(ReplaceAll(Name, Rule.Find, Rule.Replace, Rule.bMatchCase));
        Out.append(Rule.Suffix);
        if (Rule.bNumber)
        {
            Out.append(Format("_{:0{}}", Rule.NumberStart + Index, Math::Clamp(Rule.NumberDigits, 1, kMaxNumberDigits)));
        }
        return Out;
    }

    void PlanBulkRename(const FBulkRenameRule& Rule, TVector<FBulkRenameEntry>& Entries, const TFunction<bool(FStringView)>& Exists)
    {
        for (int32 Index = 0; Index < (int32)Entries.size(); ++Index)
        {
            FBulkRenameEntry& Entry = Entries[Index];
            const FStringView Source(Entry.SourcePath.c_str(), Entry.SourcePath.size());
            const FStringView Name = EntryName(Entry);

            Entry.NewName = ApplyBulkRenameRule(Rule, Name, Index);
            Entry.TargetPath = Paths::Combine(VFS::Parent(Source), Entry.NewName);
            if (!Entry.bDirectory)
            {
                Entry.TargetPath.append(VFS::Extension(VFS::FileName(Source)));
            }

            const FStringView Target(Entry.TargetPath.c_str(), Entry.TargetPath.size());
            const bool bSamePath = EqualsIgnoreCase(Target, Source);

            if (Entry.NewName.empty())
            {
                Entry.Status = EBulkRenameStatus::Empty;
            }
            else if (FStringView(Entry.NewName).find_first_of(kInvalidNameCharacters) != FStringView::npos)
            {
                Entry.Status = EBulkRenameStatus::InvalidCharacters;
            }
            else if (FStringView(Entry.NewName) == Name)
            {
                Entry.Status = EBulkRenameStatus::Unchanged;
            }
            else if (Algo::AnyOf(TSpan<const FBulkRenameEntry>(Entries.data(), Index), [Target](const FBulkRenameEntry& Earlier)
                {
                    return EqualsIgnoreCase(FStringView(Earlier.TargetPath.c_str(), Earlier.TargetPath.size()), Target);
                }))
            {
                Entry.Status = EBulkRenameStatus::Duplicate;
            }
            else if (!bSamePath && Exists(Target))
            {
                Entry.Status = EBulkRenameStatus::Exists;
            }
            else
            {
                Entry.Status = EBulkRenameStatus::Ready;
            }
        }
    }

    void OpenBulkRenameModal(IEditorToolContext* Context, TVector<FFixedString> Paths, TFunction<void()> OnRenamed)
    {
        TVector<FBulkRenameEntry> Entries;
        Entries.reserve(Paths.size());
        for (FFixedString& Path : Paths)
        {
            FBulkRenameEntry& Entry = Entries.emplace_back();
            Entry.bDirectory = VFS::IsDirectory(Path);
            Entry.SourcePath = Move(Path);
        }

        const FString Title = Format("Rename {0} Items", Entries.size());
        const float Scale = ImGuiX::GetUIScale();
        Context->PushModal(Title, ImVec2(960.0f * Scale, 620.0f * Scale),
            [Context, Entries = Move(Entries), OnRenamed = Move(OnRenamed), Rule = FBulkRenameRule(), bStale = true]() mutable
        {
            bStale |= DrawRuleInputs(Rule);
            if (bStale)
            {
                PlanBulkRename(Rule, Entries, [](FStringView Path) { return VFS::Exists(Path); });
                bStale = false;
            }

            ImGui::Spacing();
            DrawPreview(Entries);

            TVector<FBulkRenameEntry> Ready;
            for (const FBulkRenameEntry& Entry : Entries)
            {
                if (Entry.Status == EBulkRenameStatus::Ready)
                {
                    Ready.push_back(Entry);
                }
            }

            ImGui::TextColored(EditorColors::TextDim(), "%d of %d will be renamed.", (int32)Ready.size(), (int32)Entries.size());

            ImGui::BeginDisabled(Ready.empty());
            const bool bRename = ImGui::Button("Rename");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                return true;
            }

            if (!bRename)
            {
                return false;
            }

            TVector<FString> ReadOnly;
            for (const FBulkRenameEntry& Entry : Ready)
            {
                for (FString& File : AssetOps::FindReadOnlyFiles(Entry.SourcePath))
                {
                    ReadOnly.push_back(Move(File));
                }
            }

            const auto Run = [Ready, OnRenamed]() { RunBulkRename(Ready, OnRenamed); };
            if (Context->ConfirmWritable(Move(ReadOnly), Run, false))
            {
                Run();
            }
            return true;
        });
    }
}
