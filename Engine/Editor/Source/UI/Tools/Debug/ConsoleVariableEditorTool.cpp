#include "ConsoleVariableEditorTool.h"


#include "Core/Console/ConsoleVariable.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        const char* TypeLabel(const CVarValueType& Value)
        {
            return visit([]<typename T0>(T0&&) -> const char*
            {
                using T = std::decay_t<T0>;
                if constexpr (std::is_same_v<T, bool>)         return "bool";
                else if constexpr (std::is_same_v<T, int32>)   return "int";
                else if constexpr (std::is_same_v<T, float>)   return "float";
                else if constexpr (std::is_same_v<T, FStringView>) return "string";
                else                                             return "?";
            }, Value);
        }

        ImVec4 TypeColor(const CVarValueType& Value)
        {
            return visit([]<typename T0>(T0&&) -> ImVec4
            {
                using T = std::decay_t<T0>;
                if constexpr (std::is_same_v<T, bool>)         return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
                else if constexpr (std::is_same_v<T, int32>)   return ImVec4(0.55f, 0.75f, 1.00f, 1.0f);
                else if constexpr (std::is_same_v<T, float>)   return ImVec4(1.00f, 0.80f, 0.45f, 1.0f);
                else if constexpr (std::is_same_v<T, FStringView>) return ImVec4(0.90f, 0.65f, 0.95f, 1.0f);
                else                                             return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            }, Value);
        }
    }

    void FConsoleVariableEditorTool::OnInitialize()
    {
        CreateToolWindow("Console Variables", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FConsoleVariableEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FConsoleVariableEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this is",
            "Browse and edit every registered console variable. Changes fire the variable's OnChange "
            "callback immediately, same code path as typing the command at the console prompt.");
        DrawHelpTextRow("Variables vs Commands",
            "Variables hold a typed value (bool/int/float/string) you can edit inline. Commands are "
            "zero-argument actions, click Execute to run.");
        DrawHelpTextRow("Search",
            "The filter matches against both the variable name and its description, case-insensitive.");
        DrawHelpTextRow("Reset",
            "Reset restores the value the cvar was registered with. It also fires OnChange.");
    }

    bool FConsoleVariableEditorTool::PassesFilter(FStringView Name, FStringView Hint) const
    {
        if (SearchFilter.empty())
        {
            return true;
        }

        return ImGuiX::PassSearchFilter(SearchFilter, Name) || ImGuiX::PassSearchFilter(SearchFilter, Hint);
    }

    void FConsoleVariableEditorTool::DrawWindow(bool bIsFocused)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

        const FConsoleRegistry& Registry = FConsoleRegistry::Get();
        const uint32 NumVars = (uint32)Registry.GetAll().size();
        const uint32 NumCmds = (uint32)Registry.GetAllCommands().size();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.15f, 1.0f));
        ImGui::BeginChild("##CVarFilterBar", ImVec2(0, 50.0f), true, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 220.0f);
            if (ImGui::InputTextWithHint("##Search", LE_ICON_MAGNIFY " Filter by name or description...", SearchBuffer, IM_ARRAYSIZE(SearchBuffer)))
            {
                SearchFilter = SearchBuffer;
            }

            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%u vars, %u cmds", NumVars, NumCmds);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        if (ImGui::BeginTabBar("##CVarTabs", ImGuiTabBarFlags_None))
        {
            if (ImGui::BeginTabItem(LE_ICON_TUNE " Variables"))
            {
                DrawVariablesTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(LE_ICON_PLAY " Commands"))
            {
                DrawCommandsTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::PopStyleVar(2);
    }

    void FConsoleVariableEditorTool::DrawVariablesTab()
    {
        FConsoleRegistry& Registry = FConsoleRegistry::Get();
        const FConsoleRegistry::FConsoleContainer& Container = Registry.GetAll();

        TVector<const FConsoleVariable*> Visible;
        Visible.reserve(Container.size());
        for (const auto& [Name, Var] : Container)
        {
            if (PassesFilter(Var.Name, Var.Hint))
            {
                Visible.push_back(&Var);
            }
        }

        Algo::Sort(Visible.begin(), Visible.end(), [](const FConsoleVariable* A, const FConsoleVariable* B)
        {
            return A->Name < B->Name;
        });

        ImGuiTableFlags Flags =
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##CVarTable", 5, Flags, ImVec2(0, 0)))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableSetupColumn("Value",       ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("",            ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableHeadersRow();

        for (const FConsoleVariable* Var : Visible)
        {
            ImGui::TableNextRow();
            ImGui::PushID(Var->Name.data());

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.85f, 1.00f, 0.85f, 1.0f), "%.*s", (int)Var->Name.size(), Var->Name.data());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(TypeColor(*Var->ValuePtr), "%s", TypeLabel(*Var->ValuePtr));

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);

            visit([&]<typename T0>(T0& Value)
            {
                using T = std::decay_t<T0>;

                if constexpr (std::is_same_v<T, bool>)
                {
                    bool Edit = Value;
                    if (ImGui::Checkbox("##Val", &Edit))
                    {
                        Registry.SetValueFromString(Var->Name, Edit ? "true" : "false");
                    }
                }
                else if constexpr (std::is_same_v<T, int32>)
                {
                    int32 Edit = Value;
                    if (ImGui::DragInt("##Val", &Edit))
                    {
                        Registry.SetValueFromString(Var->Name, Format("{}", Edit).c_str());
                    }
                }
                else if constexpr (std::is_same_v<T, float>)
                {
                    float Edit = Value;
                    if (ImGui::DragFloat("##Val", &Edit, 0.1f, 0.0f, 0.0f, "%.3f"))
                    {
                        Registry.SetValueFromString(Var->Name, Format("{}", Edit).c_str());
                    }
                }
                else if constexpr (std::is_same_v<T, FStringView>)
                {
                    FString Key(Var->Name.data(), Var->Name.size());
                    FString& Buffer = StringEditBuffers[Key];
                    if (!ImGui::IsItemActive())
                    {
                        Buffer.assign(Value.data(), Value.size());
                    }

                    char Tmp[512];
                    size_t Copy = std::min<size_t>(sizeof(Tmp) - 1, Buffer.size());
                    std::memcpy(Tmp, Buffer.data(), Copy);
                    Tmp[Copy] = '\0';

                    if (ImGui::InputText("##Val", Tmp, sizeof(Tmp), ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        Buffer = Tmp;
                        Registry.SetValueFromString(Var->Name, FStringView(Buffer.data(), Buffer.size()));
                    }
                    else
                    {
                        Buffer = Tmp;
                    }
                }
            }, *Var->ValuePtr);

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Reset"))
            {
                visit([&]<typename T0>(const T0& DefaultValue)
                {
                    using T = std::decay_t<T0>;

                    if constexpr (std::is_same_v<T, bool>)
                    {
                        Registry.SetValueFromString(Var->Name, DefaultValue ? "true" : "false");
                    }
                    else if constexpr (std::is_same_v<T, int32> || std::is_same_v<T, float>)
                    {
                        Registry.SetValueFromString(Var->Name, Format("{}", DefaultValue).c_str());
                    }
                    else if constexpr (std::is_same_v<T, FStringView>)
                    {
                        Registry.SetValueFromString(Var->Name, DefaultValue);
                    }
                }, Var->DefaultValue);
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%.*s", (int)Var->Hint.size(), Var->Hint.data());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    void FConsoleVariableEditorTool::DrawCommandsTab()
    {
        FConsoleRegistry& Registry = FConsoleRegistry::Get();
        const FConsoleRegistry::FCommandContainer& Container = Registry.GetAllCommands();

        TVector<const FConsoleCommand*> Visible;
        Visible.reserve(Container.size());
        for (const auto& [Name, Cmd] : Container)
        {
            if (PassesFilter(Cmd.Name, Cmd.Hint))
            {
                Visible.push_back(&Cmd);
            }
        }

        Algo::Sort(Visible.begin(), Visible.end(), [](const FConsoleCommand* A, const FConsoleCommand* B)
        {
            return A->Name < B->Name;
        });

        ImGuiTableFlags Flags =
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##CmdTable", 3, Flags, ImVec2(0, 0)))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("",            ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.70f);
        ImGui::TableHeadersRow();

        for (const FConsoleCommand* Cmd : Visible)
        {
            ImGui::TableNextRow();
            ImGui::PushID(Cmd->Name.data());

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.85f, 1.00f, 0.85f, 1.0f), "%.*s", (int)Cmd->Name.size(), Cmd->Name.data());

            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton(LE_ICON_PLAY " Execute"))
            {
                Registry.ExecuteCommand(Cmd->Name);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%.*s", (int)Cmd->Hint.size(), Cmd->Hint.data());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}
