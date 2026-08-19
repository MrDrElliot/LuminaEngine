#include "PluginBrowserEditorTool.h"

#include "Core/Application/Application.h"
#include "Core/Engine/Engine.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginDescriptor.h"
#include "Core/Plugin/PluginManager.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"

#include "Paths/Paths.h"
#include "LuminaEditor.h"
#include "Tools/UI/ImGui/ImGuiAllocator.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "UI/Tools/EditorToolContext.h"

#include "imgui.h"
#include "nlohmann/json.hpp"

namespace Lumina
{
    const char* FPluginBrowserEditorTool::GetTitlebarIcon() const
    {
        return LE_ICON_PUZZLE;
    }

    void FPluginBrowserEditorTool::OnInitialize()
    {
        CreateToolWindow("Plugins", [this](bool bIsFocused)
        {
            DrawBrowserWindow(bIsFocused);
        });
    }

    void FPluginBrowserEditorTool::OnDeinitialize(const FUpdateContext&)
    {
    }

    void FPluginBrowserEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Plugins",
            "Each row is one discovered .lplugin under Engine/Plugins/ or "
            "<Project>/Plugins/. Engine plugins ship with the engine; project "
            "plugins live next to the .lproject.");
        DrawHelpTextRow("Enable / Disable",
            "Toggle the checkbox to stage a change. Click 'Apply & Restart' to "
            "save the override into the .lproject and restart. Module hot-reload "
            "isn't implemented, so changes only take effect on next launch.");
        DrawHelpTextRow("Scope",
            "Overrides are per-project: disabling a plugin here only disables it "
            "for the loaded project. Other projects keep using the descriptor "
            "default. With no project loaded the view is read-only.");
        DrawHelpTextRow("Modules",
            "The detail panel lists each module's type (Runtime / Editor / "
            "Developer) and loading phase. Editor modules are silently skipped "
            "in non-editor builds; Developer modules are skipped in Shipping.");
    }


    bool FPluginBrowserEditorTool::IsEffectivelyEnabled(const FPlugin* Plugin) const
    {
        auto It = PendingChanges.find(FName(Plugin->GetName()));
        return It != PendingChanges.end() ? It->second : Plugin->IsEnabled();
    }

    void FPluginBrowserEditorTool::SetPendingEnabled(FName PluginName, bool bEnabled)
    {
        const FPlugin* Plugin = FPluginManager::Get().FindPlugin(PluginName.ToString());
        if (!Plugin)
        {
            return;
        }
        // If the user re-toggles back to the current state, drop the pending
        // entry so the Apply button accurately reflects "no pending changes".
        if (Plugin->IsEnabled() == bEnabled)
        {
            PendingChanges.erase(PluginName);
        }
        else
        {
            PendingChanges[PluginName] = bEnabled;
            bChangesSavedBanner = false;
        }
    }


    void FPluginBrowserEditorTool::DrawBrowserWindow(bool /*bIsFocused*/)
    {
        DrawToolbar();
        ImGui::Separator();

        // Build the filtered + sorted view. Engine plugins first, then
        // project plugins; within each group, alphabetical by name.
        TVector<const FPlugin*> All = FPluginManager::Get().GetAllPlugins();
        TVector<FPlugin*> Visible;
        Visible.reserve(All.size());
        for (const FPlugin* P : All)
        {
            const bool bEngine = P->IsEnginePlugin();
            if (bEngine && !bShowEngine) continue;
            if (!bEngine && !bShowProject) continue;

            if (!SearchFilter.empty())
            {
                FStringView Name = P->GetName();
                if (Name.find(SearchFilter.c_str()) == FStringView::npos)
                {
                    continue;
                }
            }
            Visible.push_back(const_cast<FPlugin*>(P));
        }
        std::sort(Visible.begin(), Visible.end(),
            [](const FPlugin* A, const FPlugin* B)
            {
                if (A->IsEnginePlugin() != B->IsEnginePlugin())
                {
                    return A->IsEnginePlugin();
                }
                return A->GetName() < B->GetName();
            });

        // Two-pane split: table on top (60% height), details below.
        const float Avail = ImGui::GetContentRegionAvail().y;
        const float TableHeight = Math::Max(160.0f, Avail * 0.60f - 30.0f);

        if (ImGui::BeginChild("##PluginTable", ImVec2(0, TableHeight), false))
        {
            DrawTable(Visible);
        }
        ImGui::EndChild();

        ImGui::Separator();

        FPlugin* Selected = nullptr;
        if (!SelectedPlugin.IsNone())
        {
            Selected = FPluginManager::Get().FindPlugin(SelectedPlugin.ToString());
        }
        if (ImGui::BeginChild("##PluginDetails", ImVec2(0, -32.0f), false))
        {
            DrawDetailPanel(Selected);
        }
        ImGui::EndChild();

        ImGui::Separator();
        DrawFooter();
    }


    void FPluginBrowserEditorTool::DrawToolbar()
    {
        ImGui::SetNextItemWidth(260.0f);
        char Buf[128];
        const size_t Len = Math::Min(SearchFilter.size(), sizeof(Buf) - 1);
        memcpy(Buf, SearchFilter.c_str(), Len);
        Buf[Len] = '\0';
        if (ImGui::InputTextWithHint("##search", LE_ICON_MAGNIFY "  Search plugins", Buf, sizeof(Buf)))
        {
            SearchFilter.assign(Buf);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Engine", &bShowEngine);
        ImGui::SameLine();
        ImGui::Checkbox("Project", &bShowProject);

        ImGui::SameLine();
        const bool bHasProject = GEngine && !GEngine->GetProjectName().empty();
        if (!bHasProject) ImGui::BeginDisabled();
        if (ImGui::Button(LE_ICON_PUZZLE_PLUS " New Plugin"))
        {
            OpenCreatePluginDialog();
        }
        if (!bHasProject) ImGui::EndDisabled();
        if (!bHasProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Load a project to create a plugin.");
        }

        // Right-aligned scope hint.
        const char* ScopeHint = GEngine && !GEngine->GetProjectName().empty()
            ? "Scope: project (per-.lproject)"
            : "Scope: read-only (no project loaded)";
        const float ScopeWidth = ImGui::CalcTextSize(ScopeHint).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - ScopeWidth - 16.0f);
        ImGui::TextDisabled("%s", ScopeHint);
    }


    void FPluginBrowserEditorTool::DrawTable(const TVector<FPlugin*>& Plugins)
    {
        if (Plugins.empty())
        {
            ImGui::TextDisabled("No plugins match the current filter.");
            return;
        }

        const bool bCanToggle = GEngine && !GEngine->GetProjectName().empty();

        if (!ImGui::BeginTable("##Plugins", 5,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
        {
            return;
        }

        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Version",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (FPlugin* Plugin : Plugins)
        {
            ImGui::PushID(Plugin);
            ImGui::TableNextRow();

            // Enabled checkbox.
            ImGui::TableSetColumnIndex(0);
            bool bEffective = IsEffectivelyEnabled(Plugin);
            if (!bCanToggle) ImGui::BeginDisabled();
            if (ImGui::Checkbox("##en", &bEffective))
            {
                SetPendingEnabled(FName(Plugin->GetName()), bEffective);
            }
            if (!bCanToggle) ImGui::EndDisabled();

            // Name. Pending changes get a star prefix + amber text.
            ImGui::TableSetColumnIndex(1);
            const bool bPending = PendingChanges.find(FName(Plugin->GetName())) != PendingChanges.end();
            const FName SelectedName = SelectedPlugin;
            const bool bSelected = SelectedName == FName(Plugin->GetName());

            char Label[128];
            snprintf(Label, sizeof(Label), "%s%.*s",
                bPending ? LE_ICON_CIRCLE_MEDIUM " " : "",
                int(Plugin->GetName().size()), Plugin->GetName().data());
            if (bPending) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
            if (ImGui::Selectable(Label, bSelected, ImGuiSelectableFlags_SpanAllColumns))
            {
                SelectedPlugin = FName(Plugin->GetName());
            }
            if (bPending) ImGui::PopStyleColor();

            // Type.
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(Plugin->IsEnginePlugin() ? "Engine" : "Project");

            // Category.
            ImGui::TableSetColumnIndex(3);
            const FString& Cat = Plugin->GetDescriptor().Category;
            ImGui::TextUnformatted(Cat.empty() ? "-" : Cat.c_str());

            // Version.
            ImGui::TableSetColumnIndex(4);
            const FString& VerName = Plugin->GetDescriptor().VersionName;
            if (!VerName.empty())
            {
                ImGui::TextUnformatted(VerName.c_str());
            }
            else
            {
                ImGui::Text("%d", Plugin->GetDescriptor().Version);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }


    void FPluginBrowserEditorTool::DrawDetailPanel(FPlugin* Plugin)
    {
        if (!Plugin)
        {
            ImGui::TextDisabled("Select a plugin to see its details.");
            return;
        }

        const FPluginDescriptor& Desc = Plugin->GetDescriptor();

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
        ImGui::TextUnformatted(Desc.Name.c_str());
        ImGuiX::Font::PopFont();

        if (!Desc.Author.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("by %s", Desc.Author.c_str());
        }

        ImGui::Spacing();

        if (!Desc.Description.empty())
        {
            ImGui::TextWrapped("%s", Desc.Description.c_str());
            ImGui::Spacing();
        }

        if (ImGui::BeginTable("##PluginMeta", 2, ImGuiTableFlags_SizingStretchProp))
        {
            auto Row = [](const char* Key, const char* Value)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", Key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(Value && *Value ? Value : "-");
            };

            Row("Path",     Plugin->GetDescriptorPath().data());
            Row("Category", Desc.Category.c_str());
            Row("Version",  Desc.VersionName.empty()
                                ? std::to_string(Desc.Version).c_str()
                                : Desc.VersionName.c_str());
            Row("Type",     Plugin->IsEnginePlugin() ? "Engine plugin" : "Project plugin");
            Row("Editor-only", Desc.bEditorOnly ? "Yes" : "No");
            Row("Has content", Desc.bContainsContent ? "Yes" : "No");

            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Modules", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (Desc.Modules.empty())
            {
                ImGui::TextDisabled("No native modules, content-only plugin.");
            }
            else if (ImGui::BeginTable("##Modules", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Loading Phase");
                ImGui::TableHeadersRow();

                for (const FPluginModuleDescriptor& Mod : Desc.Modules)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(Mod.Name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(LexToString(Mod.Type).data());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(LexToString(Mod.LoadingPhase).data());
                }
                ImGui::EndTable();
            }
        }

        if (!Desc.Dependencies.empty())
        {
            if (ImGui::CollapsingHeader("Dependencies", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const FPluginDependency& Dep : Desc.Dependencies)
                {
                    const FPlugin* DepPlugin = FPluginManager::Get().FindPlugin(Dep.Name);
                    const char* Status = !DepPlugin   ? "missing"
                                      : !DepPlugin->IsEnabled() ? "disabled"
                                      :                            "ok";
                    ImGui::BulletText("%s (v>=%d)%s%s",
                        Dep.Name.c_str(), Dep.Version,
                        Dep.bOptional ? "  [optional]" : "",
                        Status[0] == 'o' ? "" : (FString("  -- ") + Status).c_str());
                }
            }
        }
    }


    void FPluginBrowserEditorTool::DrawFooter()
    {
        const int32 NumPending = (int32)PendingChanges.size();
        const bool bCanApply   = NumPending > 0 && GEngine && !GEngine->GetProjectName().empty();

        if (bChangesSavedBanner)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
                LE_ICON_CHECK " Changes saved. Restart the editor to apply.");
            ImGui::SameLine();
        }
        else if (NumPending > 0)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                LE_ICON_CIRCLE_MEDIUM " %d pending change%s",
                NumPending, NumPending == 1 ? "" : "s");
            ImGui::SameLine();
        }
        else
        {
            ImGui::TextDisabled("No pending changes.");
            ImGui::SameLine();
        }

        const float ButtonWidth = 180.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - ButtonWidth - 16.0f);

        if (!bCanApply) ImGui::BeginDisabled();
        if (ImGui::Button(LE_ICON_REFRESH " Apply & Restart", ImVec2(ButtonWidth, 0)))
        {
            FString Error;
            if (ApplyAndPersist(Error))
            {
                PromptRestart();
            }
            else
            {
                LOG_ERROR("[PluginBrowser] Failed to persist plugin overrides: {}", Error);
                bChangesSavedBanner = false;
            }
        }
        if (!bCanApply) ImGui::EndDisabled();
    }


    bool FPluginBrowserEditorTool::ApplyAndPersist(FString& OutError)
    {
        if (!GEngine || GEngine->GetProjectName().empty())
        {
            OutError = "No project loaded; nowhere to write plugin overrides.";
            return false;
        }

        // Compose <ProjectPath>/<ProjectName>.lproject.
        const FStringView PathView = GEngine->GetProjectPath();
        const FStringView NameView = GEngine->GetProjectName();

        FFixedString Composed = Paths::Combine(PathView, NameView);
        Composed.append(".lproject");
        Paths::Normalize(Composed);

        const FString LprojPath(Composed.data(), Composed.size());

        FString Json;
        if (!FileHelper::LoadFileIntoString(Json, LprojPath))
        {
            OutError = "Could not read ";
            OutError += LprojPath;
            return false;
        }

        nlohmann::json Root;
        try
        {
            Root = nlohmann::json::parse(Json.c_str(), Json.c_str() + Json.size());
        }
        catch (const std::exception& E)
        {
            OutError = "Malformed .lproject JSON: ";
            OutError += E.what();
            return false;
        }
        if (!Root.is_object())
        {
            OutError = ".lproject root must be a JSON object.";
            return false;
        }

        // Merge pending changes into the Plugins array; preserves existing entries
        // (and any forward-compatible fields) and appends new ones.
        nlohmann::json& PluginsArr = Root["Plugins"];
        if (!PluginsArr.is_array())
        {
            PluginsArr = nlohmann::json::array();
        }

        for (const auto& Pair : PendingChanges)
        {
            const FString Name(Pair.first.ToString().data(), Pair.first.ToString().size());
            const bool   bEnabled = Pair.second;

            bool bUpdatedExisting = false;
            for (auto& Entry : PluginsArr)
            {
                if (!Entry.is_object()) continue;
                auto NameIt = Entry.find("Name");
                if (NameIt == Entry.end() || !NameIt->is_string()) continue;
                if (NameIt->get_ref<const std::string&>() == Name.c_str())
                {
                    Entry["Enabled"] = bEnabled;
                    bUpdatedExisting = true;
                    break;
                }
            }
            if (!bUpdatedExisting)
            {
                nlohmann::json New;
                New["Name"]    = std::string(Name.c_str(), Name.size());
                New["Enabled"] = bEnabled;
                PluginsArr.push_back(Move(New));
            }
        }

        const std::string Serialized = Root.dump(4);
        const FStringView SerializedView(Serialized.c_str(), Serialized.size());
        if (!FileHelper::SaveStringToFile(SerializedView, LprojPath))
        {
            OutError = "Failed to write ";
            OutError += LprojPath;
            return false;
        }
        
        PendingChanges.clear();
        bChangesSavedBanner = true;
        LOG_INFO("[PluginBrowser] Wrote plugin overrides to {}", LprojPath);
        return true;
    }

    void FPluginBrowserEditorTool::PromptRestart()
    {
        ToolContext->PushModal("Restart Required", ImVec2(420, 160),
            []() -> bool
            {
                ImGui::TextWrapped("Plugin changes were saved to the .lproject. "
                                   "Restart the editor to load the new module set.");
                ImGui::Spacing();

                const char* RestartLabel = LE_ICON_POWER " Restart Now";
                const char* LaterLabel   = "Later";

                const ImGuiStyle& Style = ImGui::GetStyle();

                // Sized from the labels. A fixed width clips the text once the font scales with DPI.
                const float RestartWidth = ImGui::CalcTextSize(RestartLabel).x;
                const float LaterWidth   = ImGui::CalcTextSize(LaterLabel).x;
                const float ButtonWidth  = (RestartWidth > LaterWidth ? RestartWidth : LaterWidth)
                                         + Style.FramePadding.x * 2.0f;

                const float TotalWidth = ButtonWidth * 2.0f + Style.ItemSpacing.x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - TotalWidth) * 0.5f);

                if (ImGui::Button(RestartLabel, ImVec2(ButtonWidth, 0)))
                {
                    FApplication::RequestExit();
                    return true;
                }
                ImGui::SameLine();
                if (ImGui::Button(LaterLabel, ImVec2(ButtonWidth, 0)))
                {
                    return true;
                }
                return false;
            });
    }


    void FPluginBrowserEditorTool::OpenCreatePluginDialog()
    {
        NewPluginNameBuf[0] = '\0';
        NewPluginDescBuf[0] = '\0';
        NewPluginError.clear();
        CreatePluginResult.clear();

        ToolContext->PushModal("Create Plugin", ImVec2(480, 290),
            [this]() -> bool
            {
                // Confirmation view, shown after a successful create.
                if (!CreatePluginResult.empty())
                {
                    ImGui::TextWrapped("%s", CreatePluginResult.c_str());
                    ImGui::Spacing();
                    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 120.0f) * 0.5f);
                    if (ImGui::Button("OK", ImVec2(120, 0)))
                    {
                        CreatePluginResult.clear();
                        return true;
                    }
                    return false;
                }

                ImGui::TextDisabled("Creates <Project>/Plugins/<Name>/ with a Runtime + Editor module pair.");
                ImGui::Spacing();

                ImGui::TextUnformatted("Plugin name");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##pluginname", "e.g. Combat", NewPluginNameBuf, sizeof(NewPluginNameBuf));

                ImGui::TextUnformatted("Description");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##plugindesc", "Optional", NewPluginDescBuf, sizeof(NewPluginDescBuf));

                if (!NewPluginError.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.96f, 0.36f, 0.38f, 1.0f), "%s", NewPluginError.c_str());
                }

                ImGui::Spacing();
                const float ButtonWidth = 130.0f;
                const float TotalWidth  = ButtonWidth * 2.0f + 12.0f;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - TotalWidth) * 0.5f);

                const bool bNameEmpty = NewPluginNameBuf[0] == '\0';
                if (bNameEmpty) ImGui::BeginDisabled();
                if (ImGui::Button(LE_ICON_PUZZLE_PLUS " Create", ImVec2(ButtonWidth, 0)))
                {
                    FFixedString OutDir;
                    FString Error;
                    if (GEditorEngine->CreatePlugin(NewPluginNameBuf, NewPluginDescBuf, OutDir, Error))
                    {
                        LOG_INFO("[PluginBrowser] Created plugin at {}", OutDir.c_str());

                        // Regenerate the project so the new modules join its
                        // .slnx, then prompt a rebuild + restart to load them.
                        GEditorEngine->GenerateProjectFiles(GEngine->GetProjectPath());

                        CreatePluginResult = "Created plugin '";
                        CreatePluginResult += NewPluginNameBuf;
                        CreatePluginResult += "'.\n\nProject files are regenerating. Rebuild the solution "
                                              "and restart the editor to load the new modules.";
                        NewPluginError.clear();
                    }
                    else
                    {
                        NewPluginError = Error;
                    }
                }
                if (bNameEmpty) ImGui::EndDisabled();

                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0)))
                {
                    NewPluginError.clear();
                    return true;
                }
                return false;
            });
    }
}
