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

        // A category divider: bold, accent-tinted label with breathing room above/below.
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

        // A settings entry: rounded hover/selection background, a left accent bar + accent text when
        // selected. Returns true when clicked.
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

            // End the row on a real (zero-size) item so IsSetPos is cleared; otherwise a bare trailing
            // SetCursorScreenPos on the last row trips ImGui's extend-bounds assert at End()/EndChild.
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

    void FSettingsEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
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
        auto It = Tables.find(SettingsClass);
        if (It != Tables.end())
        {
            return It->second.get();
        }

        CObject* CDO = SettingsClass->GetDefaultObject();
        if (CDO == nullptr)
        {
            return nullptr;
        }

        // Edit the CDO directly (that is where the live config values live); diff/reset against the
        // pristine code-default snapshot the manager captured before loading the file.
        void* Snapshot = GConfig->GetSettingsDefault(SettingsClass);
        TUniquePtr<FPropertyTable> Table = MakeUnique<FPropertyTable>((void*)CDO, SettingsClass, Snapshot);

        CClass* Captured = SettingsClass;
        // Finish, not PostEdit: PostEdit fires on Started, Updated AND Finished, so every array
        // mutation serialized the whole CDO three times -- once BEFORE the mutation, writing the
        // pre-edit state to disk -- and each save broadcasts OnSettingsSaved, whose listeners
        // live-refresh open editors while the property table that dispatched it is still drawing.
        // A save only makes sense on the commit anyway.
        Table->SetFinishEditCallback([Captured](const FPropertyChangedEvent&)
        {
            GConfig->SaveSettings(Captured);
        });

        FPropertyTable* Raw = Table.get();
        Tables.emplace(SettingsClass, Move(Table));
        return Raw;
    }

    void FSettingsEditorTool::DrawWindow(bool bIsFocused)
    {
        if (GConfig == nullptr)
        {
            ImGui::TextDisabled("Config system not initialized.");
            return;
        }

        // Gather discovered classes once per frame.
        TVector<CClass*> Classes;
        GConfig->ForEachSettingsClass([&](CClass* Class)
        {
            Classes.push_back(Class);
        });

        if (Classes.empty())
        {
            ImGui::TextDisabled("No settings registered.");
            return;
        }

        if (SelectedClass == nullptr)
        {
            SelectedClass = Classes[0];
        }

        // Group by category so each appears exactly once (the classes arrive in discovery order, which
        // would otherwise split a category into several headers). Categories and entries are sorted.
        THashMap<FString, TVector<CClass*>> ByCategory;
        for (CClass* Class : Classes)
        {
            ByCategory[CategoryOf(Class)].push_back(Class);
        }

        TVector<FString> Categories;
        Categories.reserve(ByCategory.size());
        for (auto& Pair : ByCategory)
        {
            Categories.push_back(Pair.first);
        }
        eastl::sort(Categories.begin(), Categories.end(), [](const FString& A, const FString& B)
        {
            return strcmp(A.c_str(), B.c_str()) < 0;
        });
        for (auto& Pair : ByCategory)
        {
            eastl::sort(Pair.second.begin(), Pair.second.end(), [](CClass* A, CClass* B)
            {
                return strcmp(DisplayNameOf(A).c_str(), DisplayNameOf(B).c_str()) < 0;
            });
        }

        const float Avail = ImGui::GetContentRegionAvail().x;
        const float LeftWidth = Avail * 0.28f;

        ImGui::BeginChild("##SettingsCategories", ImVec2(LeftWidth, 0), true);
        {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
            ImGui::TextUnformatted(LE_ICON_MAGNIFY);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            SettingsFilter.Draw("##SettingsSearch", ImGui::GetContentRegionAvail().x);

            for (const FString& Category : Categories)
            {
                bool bHeaderDrawn = false;
                for (CClass* Class : ByCategory[Category])
                {
                    const FString Label = DisplayNameOf(Class);
                    if (!SettingsFilter.PassFilter(Label.c_str()))
                    {
                        continue;
                    }
                    if (!bHeaderDrawn)
                    {
                        DrawCategoryHeader(ToUpper(Category).c_str());
                        bHeaderDrawn = true;
                    }

                    ImGui::PushID(Class);
                    if (DrawSettingsEntry(Label.c_str(), SelectedClass == Class))
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
                ImGui::TextUnformatted(DisplayNameOf(SelectedClass).c_str());
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
                ImGui::Text("  %s", CategoryOf(SelectedClass).c_str());
                ImGui::PopStyleColor();

                ImGui::Separator();
                ImGui::Spacing();
                Table->DrawTree();
            }
        }
        ImGui::EndChild();
    }
}
