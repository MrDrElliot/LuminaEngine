#include "ScriptEditorTool.h"

#include "Core/Delegates/CoreDelegates.h"
#include "FileSystem/FileSystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Settings/EditorSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "UI/Tools/EditorToolContext.h"

namespace Lumina
{
    namespace ScriptEditorToolInternal
    {
        constexpr const char* WindowName = "Script";

        FString DisplayNameOf(FStringView Path)
        {
            return FString(VFS::FileName(Path));
        }

        bool SamePath(FStringView A, FStringView B)
        {
            if (A.size() != B.size())
            {
                return false;
            }
            for (size_t i = 0; i < A.size(); ++i)
            {
                const char Left = A[i] == '\\' ? '/' : CodeText::ToLower(A[i]);
                const char Right = B[i] == '\\' ? '/' : CodeText::ToLower(B[i]);
                if (Left != Right)
                {
                    return false;
                }
            }
            return true;
        }
    }

    using namespace ScriptEditorToolInternal;

    FScriptEditorTool::FScriptEditorTool(IEditorToolContext* Context, FStringView InVirtualPath)
        : FAssetEditorTool(Context, DisplayNameOf(InVirtualPath))
        , VirtualPath(InVirtualPath)
    {
        DiskPath = VFS::ResolvePath(InVirtualPath).c_str();
        Language.SetFilePath(DiskPath.View());

        FCodeEditorSettings& Settings = CodeEditor.GetSettings();
        Settings.FontScale = GetDefault<CScriptEditorSettings>()->FontScale;
        Settings.TabSize = 4;
        Settings.bInsertSpaces = true;
    }

    void FScriptEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CodeEditor.SetLanguage(&Language);
        CodeEditor.SetDefinitionHandler([this](const FCodeLocation& Location)
        {
            OpenDefinition(Location);
        });
        LoadFromDisk();
        Language.Warm(CodeEditor.GetDocument());
        StartWatching();

        FileRenamedHandle = FCoreDelegates::OnContentFileRenamed.AddLambda([this](FStringView Old, FStringView New)
        {
            if (Old == VirtualPath.View())
            {
                VirtualPath = New;
                DiskPath = VFS::ResolvePath(New).c_str();
                Language.SetFilePath(DiskPath.View());
            }
        });

        CreateToolWindow(WindowName, [this](bool bFocused)
        {
            DrawToolbar();
            ImGui::Separator();

            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const float StatusHeight = ImGui::GetFrameHeightWithSpacing();
            CodeEditor.Render("##script", ImVec2(Avail.x, Math::Max(32.0f, Avail.y - StatusHeight)));

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
            {
                bRequestGoto = true;
            }
            DrawStatusBar();
        });
    }

    void FScriptEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        FCoreDelegates::OnContentFileRenamed.Remove(FileRenamedHandle);
        FileWatcher.Stop();
    }

    void FScriptEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (bExternalChangePending.exchange(false, Atomic::MemoryOrderAcquire))
        {
            if (!bDirty)
            {
                LoadFromDisk();
            }
            else
            {
                ImGuiX::Notifications::NotifyWarning("'{}' changed on disk while it has unsaved edits here; keeping the edits.", VirtualPath.c_str());
            }
        }

        if (CheckedVersion != CodeEditor.GetVersion())
        {
            CheckedVersion = CodeEditor.GetVersion();
            bDirty = CodeEditor.GetText() != SavedText;
        }

        if (PendingReveal.Line >= 0)
        {
            CodeEditor.SetCursor(PendingReveal);
            CodeEditor.Focus();
            PendingReveal.Line = -1;
        }

        if (ReloadsBeforeSave >= 0 && DotNet::GetFinishedScriptReloads() > ReloadsBeforeSave)
        {
            ReloadsBeforeSave = -1;
            if (DotNet::GetLastScriptReloadResult() == 0)
            {
                ImGuiX::Notifications::NotifySuccess("Scripts reloaded.");
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Scripts did not compile, so the last build that worked is still running.");
            }
        }
    }

    void FScriptEditorTool::OnSave()
    {
        const FString Text = CodeEditor.GetText();
        if (!VFS::WriteFile(VirtualPath.View(), Text.View()))
        {
            ImGuiX::Notifications::NotifyError("Failed to save '{}'.", VirtualPath.c_str());
            return;
        }
        SavedText = Text;
        bDirty = false;
        ReloadsBeforeSave = DotNet::GetFinishedScriptReloads();
        DotNet::NotifyScriptSourceChanged(DiskPath.View());
        ImGuiX::Notifications::NotifyInfo("Saved '{}', reloading scripts.", DisplayNameOf(VirtualPath.View()).c_str());
    }

    void FScriptEditorTool::LoadFromDisk()
    {
        FString Text;
        if (!VFS::ReadFile(Text, VirtualPath.View()))
        {
            ImGuiX::Notifications::NotifyError("Could not read '{}'.", VirtualPath.c_str());
            return;
        }
        Text.erase(Algo::Remove(Text.begin(), Text.end(), '\r'), Text.end());
        SavedText = Text;
        bDirty = false;
        if (Text != CodeEditor.GetText())
        {
            CodeEditor.SetText(Text.View());
        }
        CheckedVersion = CodeEditor.GetVersion();
    }

    void FScriptEditorTool::StartWatching()
    {
        const FStringView Folder = VFS::Parent(DiskPath.View(), true);
        const FString FileName(VFS::FileName(DiskPath.View()));
        if (Folder.empty())
        {
            return;
        }
        FileWatcher.Watch(FFixedString(Folder.data(), Folder.size()), [this, FileName](const FFileEvent& Event)
        {
            const bool bChanged = Event.Action == EFileAction::Modified || Event.Action == EFileAction::Added;
            if (bChanged && SamePath(VFS::FileName(Event.Path.View()), FileName.View()))
            {
                bExternalChangePending.store(true, Atomic::MemoryOrderRelease);
            }
        }, false);
    }

    void FScriptEditorTool::OpenDefinition(const FCodeLocation& Location)
    {
        if (SamePath(Location.Path.View(), DiskPath.View()))
        {
            CodeEditor.SetCursor(Location.Position);
            return;
        }
        const FFixedString Target = VFS::ResolveToVirtualPath(Location.Path.View());
        if (Target.empty())
        {
            ImGuiX::Notifications::NotifyWarning("'{}' is outside the project's script folders.", Location.Path.c_str());
            return;
        }
        const FStringView TargetView(Target.c_str(), Target.size());
        ToolContext->OpenFileEditor(TargetView);
        FEditorTool* Opened = ToolContext->FindFileEditor(TargetView);
        if (Opened != nullptr && Opened->GetUniqueTypeID() == FScriptEditorTool::s_toolTypeID)
        {
            static_cast<FScriptEditorTool*>(Opened)->Reveal(Location.Position);
        }
    }

    void FScriptEditorTool::DrawToolbar()
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::TextColored(EditorColors::SectionHeader(), "%s", VirtualPath.c_str());
        ImGuiX::Font::PopFont();
        if (bDirty)
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::Warning(), "  unsaved");
        }

        if (ImGui::Button(LE_ICON_CONTENT_SAVE " Save"))
        {
            OnSave();
        }
        ImGui::SetItemTooltip("Save and reload scripts (Ctrl+S).");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Revert"))
        {
            CodeEditor.SetText({});
            LoadFromDisk();
        }
        ImGui::SetItemTooltip("Discard edits and read the file again.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!CodeEditor.CanUndo());
        if (ImGui::Button(LE_ICON_UNDO))
        {
            CodeEditor.Undo();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Undo (Ctrl+Z).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!CodeEditor.CanRedo());
        if (ImGui::Button(LE_ICON_REDO))
        {
            CodeEditor.Redo();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Redo (Ctrl+Y).");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_MAGNIFY " Find"))
        {
            CodeEditor.OpenFind(false);
        }
        ImGui::SetItemTooltip("Find (Ctrl+F), or find and replace (Ctrl+H).");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_FORMAT_LINE_SPACING " Go to"))
        {
            bRequestGoto = true;
        }
        ImGui::SetItemTooltip("Go to a line (Ctrl+G). F12 or Ctrl+Click goes to a definition.");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CODE_BRACES " View"))
        {
            ImGui::OpenPopup("##script_view");
        }
        if (ImGui::BeginPopup("##script_view"))
        {
            FCodeEditorSettings& Settings = CodeEditor.GetSettings();
            ImGui::SliderFloat("Font scale", &Settings.FontScale, 0.75f, 3.0f, "%.2fx");
            ImGui::Checkbox("Suggest while typing", &Settings.bSuggestWhileTyping);
            ImGui::Checkbox("Line numbers", &Settings.bShowLineNumbers);
            ImGui::Checkbox("Minimap", &Settings.bShowMiniMap);
            ImGui::Checkbox("Indent guides", &Settings.bShowIndentGuides);
            ImGui::Checkbox("Whitespace", &Settings.bShowWhitespace);
            ImGui::Checkbox("Highlight occurrences", &Settings.bHighlightOccurrences);
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Reload Scripts"))
        {
            DotNet::RequestScriptReload();
        }
        ImGui::SetItemTooltip("Recompile and reload every script now.");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_OPEN_IN_NEW " Open Externally"))
        {
            Platform::LaunchURL(UTF8_TO_TCHAR(DiskPath.c_str()));
        }
        ImGui::SetItemTooltip("Open the file in the application the system uses for .cs files.");

        if (bRequestGoto)
        {
            ImGui::OpenPopup("##script_goto");
            GotoLineBuffer = CodeEditor.GetCursor().Line + 1;
            bRequestGoto = false;
        }
        DrawGotoLinePopup();
    }

    void FScriptEditorTool::DrawGotoLinePopup()
    {
        if (!ImGui::BeginPopup("##script_goto"))
        {
            return;
        }
        ImGui::TextDisabled("Go to line (1 - %d)", CodeEditor.GetLineCount());
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        const bool bSubmit = ImGui::InputInt("##line", &GotoLineBuffer, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Go") || bSubmit)
        {
            CodeEditor.SetCursor({ Math::Clamp(GotoLineBuffer, 1, CodeEditor.GetLineCount()) - 1, 0 });
            CodeEditor.Focus();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void FScriptEditorTool::DrawStatusBar()
    {
        const FCodePosition Cursor = CodeEditor.GetCursor();
        const int32 Column = CodeText::VisualColumn(CodeEditor.GetDocument().GetLineView(Cursor.Line), Cursor.Column, CodeEditor.GetSettings().TabSize);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::TextDim(), "Ln %d, Col %d", Cursor.Line + 1, Column + 1);
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextColored(EditorColors::TextDim(), "%d lines", CodeEditor.GetLineCount());
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextColored(EditorColors::Success(), LE_ICON_LANGUAGE_CSHARP " C#");

        int32 Errors = 0;
        int32 Warnings = 0;
        const FCodeDiagnostic* First = nullptr;
        for (const FCodeDiagnostic& Diagnostic : CodeEditor.GetDiagnostics())
        {
            Errors += Diagnostic.Severity == ECodeSeverity::Error ? 1 : 0;
            Warnings += Diagnostic.Severity == ECodeSeverity::Warning ? 1 : 0;
            First = First == nullptr && Diagnostic.Severity != ECodeSeverity::Info ? &Diagnostic : First;
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (Errors == 0 && Warnings == 0)
        {
            ImGui::TextColored(EditorColors::Success(), LE_ICON_CHECK_CIRCLE_OUTLINE " No problems");
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Errors > 0 ? EditorColors::Danger() : EditorColors::Warning());
        char Label[64];
        std::snprintf(Label, sizeof(Label), LE_ICON_ALERT_CIRCLE_OUTLINE " %d errors, %d warnings", Errors, Warnings);
        if (ImGui::SmallButton(Label) && First != nullptr)
        {
            CodeEditor.Select(First->Range);
            CodeEditor.Focus();
        }
        ImGui::PopStyleColor();
        if (First != nullptr)
        {
            ImGui::SetItemTooltip("%s", First->Message.c_str());
        }
    }
}
