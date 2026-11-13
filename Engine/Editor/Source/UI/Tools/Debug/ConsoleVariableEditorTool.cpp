#include "ConsoleVariableEditorTool.h"


#include "Core/Console/ConsoleVariable.h"
#include "Containers/StringFormat.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    namespace
    {
        const char* TypeLabel(const CVarValueType& Value)
        {
            return Visit([]<typename T0>(T0&&) -> const char*
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
            return Visit([]<typename T0>(T0&&) -> ImVec4
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
        if (SearchFilter.IsEmpty())
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
        const uint32 NumVars = (uint32)Registry.GetAll().Num();
        const uint32 NumCmds = (uint32)Registry.GetAllCommands().Num();

        const FFixedString Counts = FormatAs<FFixedString>("{} variables, {} commands", NumVars, NumCmds);
        const float CountsWidth = ImGui::CalcTextSize(*Counts).x;

        ImGui::SetNextItemWidth(Math::Max(ImGui::GetContentRegionAvail().x - CountsWidth - ImGui::GetStyle().ItemSpacing.x, 120.0f));
        ImGuiX::InputText("##Search", SearchFilter, ImGuiInputTextFlags_None, LE_ICON_MAGNIFY " Filter by name or description...");

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", *Counts);

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
        Visible.Reserve(Container.Num());
        for (const auto& [Name, Var] : Container)
        {
            if (PassesFilter(Var.Name, Var.Hint))
            {
                Visible.Add(&Var);
            }
        }

        Algo::Sort(Visible, [](const FConsoleVariable* A, const FConsoleVariable* B)
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
            ImGui::PushID(Var->Name.GetData());

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.85f, 1.00f, 0.85f, 1.0f), "%.*s", Var->Name.Len(), Var->Name.GetData());

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(TypeColor(*Var->ValuePtr), "%s", TypeLabel(*Var->ValuePtr));

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);

            Visit([&]<typename T0>(T0& Value)
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
                        Registry.SetValueFromString(Var->Name, *Format("{}", Edit));
                    }
                }
                else if constexpr (std::is_same_v<T, float>)
                {
                    float Edit = Value;
                    if (ImGui::DragFloat("##Val", &Edit, 0.1f, 0.0f, 0.0f, "%.3f"))
                    {
                        Registry.SetValueFromString(Var->Name, *Format("{}", Edit));
                    }
                }
                else if constexpr (std::is_same_v<T, FStringView>)
                {
                    // ImGui keeps the in-progress text itself and writes it back only on Enter.
                    FString Edit(Value);
                    if (ImGuiX::InputText("##Val", Edit, ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        Registry.SetValueFromString(Var->Name, Edit);
                    }
                }
            }, *Var->ValuePtr);

            ImGui::TableSetColumnIndex(3);
            if (!(*Var->ValuePtr == Var->DefaultValue) && ImGui::Button("Reset", ImVec2(-FLT_MIN, 0.0f)))
            {
                Visit([&]<typename T0>(const T0& DefaultValue)
                {
                    using T = std::decay_t<T0>;

                    if constexpr (std::is_same_v<T, bool>)
                    {
                        Registry.SetValueFromString(Var->Name, DefaultValue ? "true" : "false");
                    }
                    else if constexpr (std::is_same_v<T, int32> || std::is_same_v<T, float>)
                    {
                        Registry.SetValueFromString(Var->Name, *Format("{}", DefaultValue));
                    }
                    else if constexpr (std::is_same_v<T, FStringView>)
                    {
                        Registry.SetValueFromString(Var->Name, DefaultValue);
                    }
                }, Var->DefaultValue);
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%.*s", Var->Hint.Len(), Var->Hint.GetData());
            if (ImGui::IsItemHovered() && ImGui::GetItemRectMax().x > ImGui::GetWindowPos().x + ImGui::GetWindowWidth())
            {
                ImGui::SetTooltip("%.*s", Var->Hint.Len(), Var->Hint.GetData());
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    void FConsoleVariableEditorTool::DrawCommandsTab()
    {
        FConsoleRegistry& Registry = FConsoleRegistry::Get();
        const FConsoleRegistry::FCommandContainer& Container = Registry.GetAllCommands();

        TVector<const FConsoleCommand*> Visible;
        Visible.Reserve(Container.Num());
        for (const auto& [Name, Cmd] : Container)
        {
            if (PassesFilter(Cmd.Name, Cmd.Hint))
            {
                Visible.Add(&Cmd);
            }
        }

        Algo::Sort(Visible, [](const FConsoleCommand* A, const FConsoleCommand* B)
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
            ImGui::PushID(Cmd->Name.GetData());

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.85f, 1.00f, 0.85f, 1.0f), "%.*s", Cmd->Name.Len(), Cmd->Name.GetData());

            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(LE_ICON_PLAY " Execute", ImVec2(-FLT_MIN, 0.0f)))
            {
                Registry.ExecuteCommand(Cmd->Name);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%.*s", Cmd->Hint.Len(), Cmd->Hint.GetData());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}
