#include "SettingsEditorTool.h"

#include "Config/Config.h"
#include "Core/Object/Class.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace
    {
        FString DisplayNameOf(CClass* Class)
        {
            if (Class->HasMeta("DisplayName"))
            {
                return Class->GetMeta("DisplayName");
            }
            return FConfig::GetSettingsSection(Class);
        }

        FString CategoryOf(CClass* Class)
        {
            if (Class->HasMeta("Category"))
            {
                return Class->GetMeta("Category");
            }
            return "General";
        }

        FString ToUpper(const FString& In)
        {
            FString Out = In;
            for (char& C : Out)
            {
                if (C >= 'a' && C <= 'z')
                {
                    C = static_cast<char>(C - 32);
                }
            }
            return Out;
        }

        // A category divider, bold and accent-tinted with breathing room above and below.
        void DrawCategoryHeader(const char* Label)
        {
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
            ImGui::TextUnformatted(Label);
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
        }

        // Rounded hover background with a left accent bar when selected, returning true on click.
        bool DrawSettingsEntry(const char* Label, bool bSelected)
        {
            using namespace EditorColors;
            const float  Scale  = ImGuiX::GetUIScale();
            const float  Avail  = ImGui::GetContentRegionAvail().x;
            const float  Height = 26.0f * Scale;
            const float  Round  = 4.0f * Scale;
            const ImVec2 P0     = ImGui::GetCursorScreenPos();
            const ImVec2 P1     = ImVec2(P0.x + Avail, P0.y + Height);

            ImGui::SetCursorScreenPos(P0);
            const bool bClicked = ImGui::InvisibleButton("##entry", ImVec2(Avail, Height));
            const bool bHovered = ImGui::IsItemHovered();
            if (bHovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            ImDrawList* DL = ImGui::GetWindowDrawList();
            if (bSelected || bHovered)
            {
                DL->AddRectFilled(P0, P1, U32(bSelected ? RowBgActive() : RowBgHovered()), Round);
            }
            if (bSelected)
            {
                DL->AddRectFilled(P0, ImVec2(P0.x + 3.0f * Scale, P1.y), U32(Accent()), Round);
            }

            const float TextY = P0.y + (Height - ImGui::GetFontSize()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(P0.x + 12.0f * Scale, TextY));
            ImGui::PushStyleColor(ImGuiCol_Text, bSelected ? Accent() : TextPrimary());
            ImGui::TextUnformatted(Label);
            ImGui::PopStyleColor();

            // Ending on a real zero-size item clears IsSetPos, or End() trips the extend-bounds assert.
            ImGui::SetCursorScreenPos(ImVec2(P0.x, P1.y + 2.0f * Scale));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return bClicked;
        }
    }

    void FSettingsEditorTool::OnInitialize()
    {
        CreateToolWindow("Settings", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FSettingsEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What's Listed",
            "Every CDeveloperSettings subclass. Each is drawn with the standard reflected property table, "
            "so it behaves exactly like an asset or component details panel.");
        DrawHelpTextRow("Editing",
            "Changes write immediately to the class's ConfigFile (grouped JSON). Right-click a row > "
            "'Reset to Default' restores the value declared in code.");
        DrawHelpTextRow("Adding settings",
            "Declare a CDeveloperSettings subclass with PROPERTY members and a REFLECT(ConfigFile=...) tag. "
            "It is discovered automatically; read it anywhere via GetDefault<CMySettings>().");
    }

    FPropertyTable* FSettingsEditorTool::GetOrCreateTable(CClass* SettingsClass)
    {
        auto It = Tables.Find(SettingsClass);
        if (It != nullptr)
        {
            return It->get();
        }

        CObject* CDO = SettingsClass->GetDefaultObject();
        if (CDO == nullptr)
        {
            return nullptr;
        }

        // Edits the CDO where the live values are, and diffs against the pristine code-default snapshot.
        void* Snapshot = Global::GConfig->GetSettingsDefault(SettingsClass);
        TUniquePtr<FPropertyTable> Table = MakeUnique<FPropertyTable>((void*)CDO, SettingsClass, Snapshot);

        CClass* Captured = SettingsClass;
        // PostEdit fires on Started too, which wrote the pre-edit state to disk on every array mutation.
        Table->SetFinishEditCallback([Captured](const FPropertyChangedEvent&)
        {
            Global::GConfig->SaveSettings(Captured);
        });

        FPropertyTable* Raw = Table.get();
        Tables.TryAdd(SettingsClass, Move(Table));
        return Raw;
    }

    void FSettingsEditorTool::DrawWindow(bool bIsFocused)
    {
        if (Global::GConfig == nullptr)
        {
            ImGui::TextDisabled("Config system not initialized.");
            return;
        }

        // Gather discovered classes once per frame.
        TVector<CClass*> Classes;
        Global::GConfig->ForEachSettingsClass([&](CClass* Class)
        {
            Classes.Add(Class);
        });

        if (Classes.IsEmpty())
        {
            ImGui::TextDisabled("No settings registered.");
            return;
        }

        if (SelectedClass == nullptr)
        {
            SelectedClass = Classes[0];
        }

        // Grouped so each category appears once, since discovery order would split one into several.
        THashMap<FString, TVector<CClass*>> ByCategory;
        for (CClass* Class : Classes)
        {
            ByCategory[CategoryOf(Class)].Add(Class);
        }

        TVector<FString> Categories;
        ByCategory.GenerateKeyArray(Categories);
        Algo::Sort(Categories, [](const FString& A, const FString& B)
        {
            return strcmp(*A, *B) < 0;
        });
        for (auto& [Category, CategoryClasses] : ByCategory)
        {
            Algo::Sort(CategoryClasses, [](CClass* A, CClass* B)
            {
                return strcmp(*DisplayNameOf(A), *DisplayNameOf(B)) < 0;
            });
        }

        const float Avail = ImGui::GetContentRegionAvail().x;
        const float LeftWidth = Avail * 0.28f;

        ImGui::BeginChild("##SettingsCategories", ImVec2(LeftWidth, 0), true);
        {
            ImGuiX::SearchBar("##SettingsSearch", SettingsFilter, LE_ICON_MAGNIFY " Search settings...");

            for (const FString& Category : Categories)
            {
                bool bHeaderDrawn = false;
                for (CClass* Class : ByCategory[Category])
                {
                    const FString Label = DisplayNameOf(Class);
                    if (!ImGuiX::PassSearchFilter(SettingsFilter, *Label))
                    {
                        continue;
                    }
                    if (!bHeaderDrawn)
                    {
                        DrawCategoryHeader(*ToUpper(Category));
                        bHeaderDrawn = true;
                    }

                    ImGui::PushID(Class);
                    if (DrawSettingsEntry(*Label, SelectedClass == Class))
                    {
                        SelectedClass = Class;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##SettingsDetails", ImVec2(0, 0), true);
        {
            if (FPropertyTable* Table = GetOrCreateTable(SelectedClass))
            {
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextPrimary());
                ImGui::TextUnformatted(*DisplayNameOf(SelectedClass));
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
                ImGui::Text("  %s", *CategoryOf(SelectedClass));
                ImGui::PopStyleColor();

                ImGui::Separator();
                ImGui::Spacing();
                Table->DrawTree();
            }
        }
        ImGui::EndChild();
    }
}
