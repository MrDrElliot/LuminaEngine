#include "CodeWorkspaceTool.h"

#include <imgui_internal.h>
#include "Asset/AssetOps.h"
#include "Config/Config.h"
#include "Core/Delegates/CoreDelegates.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/FileInfo.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Settings/EditorSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/CodeEditor/CodeEditorPreferences.h"
#include "UI/CodeEditor/Languages/PlainTextLanguage.h"
#include "UI/CodeEditor/Languages/RmlLanguage.h"
#include "UI/CodeEditor/Languages/SlangLanguage.h"
#include "UI/Tools/EditorToolContext.h"

#if !defined(_WIN32)
#include <strings.h>
#define _stricmp strcasecmp
#endif

namespace Lumina
{
    namespace CodeWorkspaceInternal
    {
        constexpr const char* ExplorerWindow = "Explorer";
        constexpr const char* EditorWindow = "Editor";
        constexpr const char* ProblemsWindow = "Problems";
        constexpr const char* SearchWindow = "Search";
        constexpr const char* OutlineWindow = "Outline";

        constexpr int32  MaxSearchResults = 5000;
        constexpr int32  MaxHistory = 64;
        constexpr int32  MaxPickerRows = 200;
        constexpr double ProblemsAfterSaveSeconds = 1.5;
        constexpr double ProblemsRetrySeconds = 1.0;
        constexpr int32  MaxPreview = 200;

        constexpr FStringView TextExtensions[] = { ".cs", ".slang", ".hlsl", ".rml", ".rcss", ".json", ".md", ".txt", ".ini", ".xml", ".yaml", ".yml", ".lproject", ".cfg", ".py", ".bat", ".sh" };
        // Markup keeps its own tool with a live preview, so the project routes it there and only the explorer opens it here.
        constexpr FStringView RoutedExtensions[] = { ".cs", ".slang", ".hlsl", ".json", ".md", ".txt", ".ini", ".xml", ".yaml", ".yml", ".lproject", ".cfg", ".py", ".bat", ".sh" };

        bool EndsWithIgnoreCase(FStringView Text, FStringView Suffix)
        {
            return Text.Len() >= Suffix.Len() && CodeText::StartsWithIgnoreCase(Text.Mid(Text.Len() - Suffix.Len()), Suffix);
        }

        bool HasAnyExtension(FStringView Path, TSpan<const FStringView> Extensions)
        {
            for (FStringView Extension : Extensions)
            {
                if (EndsWithIgnoreCase(Path, Extension))
                {
                    return true;
                }
            }
            return false;
        }

        bool SamePath(FStringView A, FStringView B)
        {
            if (A.Len() != B.Len())
            {
                return false;
            }
            for (int32 Index = 0; Index < A.Len(); ++Index)
            {
                const char Left = A[Index] == '\\' ? '/' : CodeText::ToLower(A[Index]);
                const char Right = B[Index] == '\\' ? '/' : CodeText::ToLower(B[Index]);
                if (Left != Right)
                {
                    return false;
                }
            }
            return true;
        }

        // A subsequence match scored higher for runs and for letters that start a word or a path segment.
        int32 FuzzyScore(FStringView Query, FStringView Text)
        {
            if (Query.IsEmpty())
            {
                return 1;
            }
            int32 Score = 0;
            int32 Run = 0;
            int32 Matched = 0;
            for (int32 Index = 0; Index < Text.Len() && Matched < Query.Len(); ++Index)
            {
                if (CodeText::ToLower(Text[Index]) != CodeText::ToLower(Query[Matched]))
                {
                    Run = 0;
                    continue;
                }
                const char Previous = Index > 0 ? Text[Index - 1] : '/';
                const bool bBoundary = Previous == '/' || Previous == '_' || Previous == '.' || Previous == ' ' || (Text[Index] >= 'A' && Text[Index] <= 'Z');
                Score += 1 + ++Run + (bBoundary ? 6 : 0);
                ++Matched;
            }
            return Matched == Query.Len() ? Score : 0;
        }

        const char* FileIcon(FStringView Path)
        {
            if (EndsWithIgnoreCase(Path, ".cs"))
            {
                return LE_ICON_LANGUAGE_CSHARP;
            }
            if (EndsWithIgnoreCase(Path, ".slang") || EndsWithIgnoreCase(Path, ".hlsl"))
            {
                return LE_ICON_FILE_CODE_OUTLINE;
            }
            if (EndsWithIgnoreCase(Path, ".json"))
            {
                return LE_ICON_CODE_JSON;
            }
            if (EndsWithIgnoreCase(Path, ".md"))
            {
                return LE_ICON_LANGUAGE_MARKDOWN_OUTLINE;
            }
            if (EndsWithIgnoreCase(Path, ".rml") || EndsWithIgnoreCase(Path, ".rcss"))
            {
                return LE_ICON_LANGUAGE_XAML;
            }
            return LE_ICON_FILE_DOCUMENT_OUTLINE;
        }

        const char* SymbolIcon(ECodeCompletionKind Kind)
        {
            switch (Kind)
            {
                case ECodeCompletionKind::Type:     return LE_ICON_CUBE_OUTLINE;
                case ECodeCompletionKind::Function: return LE_ICON_FUNCTION;
                case ECodeCompletionKind::Field:
                case ECodeCompletionKind::Variable: return LE_ICON_VARIABLE;
                case ECodeCompletionKind::Property: return LE_ICON_CODE_BRACES;
                case ECodeCompletionKind::Value:    return LE_ICON_FORMAT_LIST_BULLETED;
                case ECodeCompletionKind::Event:    return LE_ICON_LIGHTBULB_ON_OUTLINE;
                case ECodeCompletionKind::Module:   return LE_ICON_SITEMAP_OUTLINE;
                default:                            return LE_ICON_DOTS_HORIZONTAL;
            }
        }

        // Build output and tool folders, such as obj, bin and .idea, hold copies and caches rather than sources.
        bool IsInOutputFolder(FStringView Relative)
        {
            for (int32 Slash = Relative.FindFirstOf("/\\"); Slash != Constants::kIndexNone; Slash = Relative.FindFirstOf("/\\"))
            {
                const FStringView Folder = Relative.Mid(0, Slash);
                if (Folder.StartsWith('.') || SamePath(Folder, "obj") || SamePath(Folder, "bin"))
                {
                    return true;
                }
                Relative = Relative.Mid(Slash + 1);
            }
            return false;
        }

        bool IsIdentifier(FStringView Text)
        {
            if (Text.IsEmpty() || CodeText::IsDigit(Text[0]))
            {
                return false;
            }
            for (const char C : Text)
            {
                if (!(CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_' || (uint8)C >= 0x80))
                {
                    return false;
                }
            }
            return true;
        }

        FString Trimmed(FStringView Line)
        {
            const int32 Indent = CodeText::LeadingWhitespace(Line);
            FStringView Rest = Line.Mid((size_t)Indent);
            return FString(Rest.Mid(0, Math::Min(Rest.Len(), MaxPreview)));
        }

        // The innermost declaration whose body covers Line, which names where the caret is.
        const FCodeSymbolInfo* EnclosingSymbol(const TVector<FCodeSymbolInfo>& Outline, int32 Line)
        {
            const FCodeSymbolInfo* Best = nullptr;
            for (const FCodeSymbolInfo& Symbol : Outline)
            {
                if (Symbol.Position.Line <= Line && Line <= Math::Max(Symbol.EndLine, Symbol.Position.Line) && (Best == nullptr || Symbol.Depth >= Best->Depth))
                {
                    Best = &Symbol;
                }
            }
            return Best;
        }

        struct FPickerRow
        {
            int32         Score = 0;
            const char*   Icon = nullptr;
            FString       Label;
            FString       Detail;
            FString       VirtualPath;
            FCodePosition Position{ -1, 0 };
        };

        int32 ParseLineNumber(FStringView Text)
        {
            int32 Value = 0;
            for (const char C : Text)
            {
                if (!CodeText::IsDigit(C) || Value > 10000000)
                {
                    return -1;
                }
                Value = Value * 10 + (C - '0');
            }
            return Text.IsEmpty() ? -1 : Value;
        }

        // Brings a docked panel to the front of its tab bar without taking keyboard focus from the code.
        void RevealDockedWindow(const char* WindowName)
        {
            ImGuiWindow* Window = ImGui::FindWindowByName(WindowName);
            if (Window != nullptr && Window->DockNode != nullptr && Window->DockNode->TabBar != nullptr)
            {
                Window->DockNode->TabBar->NextSelectedTabId = Window->TabId;
            }
        }

        bool ToggleButton(const char* Label, bool& bValue, const char* Tooltip)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, bValue ? EditorColors::WithAlpha(EditorColors::Accent(), 0.55f) : EditorColors::Button());
            const bool bPressed = ImGui::Button(Label);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", Tooltip);
            if (bPressed)
            {
                bValue = !bValue;
            }
            return bPressed;
        }

        // A clickable row that shows a line number and the line, with the matched part drawn over a highlight.
        bool HitRow(int32 Id, int32 Line, FStringView Preview, int32 MatchColumn, int32 MatchLength, bool bDeclaration)
        {
            ImGui::PushID(Id);
            const bool bClicked = ImGui::Selectable("##hit", false, ImGuiSelectableFlags_SpanAvailWidth);
            ImGui::PopID();
            const ImVec2 Min = ImGui::GetItemRectMin();
            ImDrawList* Draw = ImGui::GetWindowDrawList();
            const ImU32 Dim = ImGui::GetColorU32(EditorColors::TextDim());
            const ImU32 Text = ImGui::GetColorU32(ImGuiCol_Text);
            float X = Min.x;

            char Number[16];
            std::snprintf(Number, sizeof(Number), "%d", Line + 1);
            Draw->AddText(ImVec2(X, Min.y), Dim, Number);
            X += ImGui::CalcTextSize("00000").x;
            if (bDeclaration)
            {
                Draw->AddText(ImVec2(X, Min.y), ImGui::GetColorU32(EditorColors::Accent()), LE_ICON_CIRCLE_MEDIUM);
            }
            X += ImGui::CalcTextSize(LE_ICON_CIRCLE_MEDIUM " ").x;

            const int32 Size = Preview.Len();
            const int32 Start = Math::Clamp(MatchColumn, 0, Size);
            const int32 End = MatchColumn < 0 ? Start : Math::Clamp(MatchColumn + MatchLength, Start, Size);
            const char* Base = Preview.GetData();
            Draw->AddText(ImVec2(X, Min.y), Text, Base, Base + Start);
            X += ImGui::CalcTextSize(Base, Base + Start).x;
            if (End > Start)
            {
                const float Width = ImGui::CalcTextSize(Base + Start, Base + End).x;
                Draw->AddRectFilled(ImVec2(X, Min.y), ImVec2(X + Width, Min.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(EditorColors::WithAlpha(EditorColors::Warning(), 0.3f)), 2.0f);
                Draw->AddText(ImVec2(X, Min.y), Text, Base + Start, Base + End);
                X += Width;
            }
            Draw->AddText(ImVec2(X, Min.y), Text, Base + End, Base + Size);
            return bClicked;
        }
    }

    using namespace CodeWorkspaceInternal;

    FCodeWorkspaceTool::FCodeWorkspaceTool(IEditorToolContext* Context)
        : FEditorTool(Context, "Code", nullptr)
    {
    }

    bool FCodeWorkspaceTool::Handles(FStringView VirtualPath)
    {
        return HasAnyExtension(VirtualPath, RoutedExtensions);
    }

    void FCodeWorkspaceTool::OnInitialize()
    {
        auto AddRoot = [this](const char* Label, FStringView VirtualPath)
        {
            if (VFS::IsDirectory(VirtualPath))
            {
                Roots.Emplace(Label, FString(VirtualPath), FString(*VFS::ResolvePath(VirtualPath)));
            }
        };
        AddRoot("Scripts", "/Game/Scripts");
        AddRoot("Content", "/Game/Content");
        AddRoot("Shaders", "/Game/Shaders");
        AddRoot("Engine Scripts", "/Engine/Resources/Scripts");
        AddRoot("Engine Shaders", "/Engine/Resources/Shaders");
        RebuildIndex();
        StartWatching();

        auto Panel = [this](void (FCodeWorkspaceTool::*Draw)())
        {
            return [this, Draw](bool bFocused)
            {
                if (bFocused)
                {
                    HandleShortcuts();
                }
                (this->*Draw)();
            };
        };
        CreateToolWindow(ExplorerWindow, Panel(&FCodeWorkspaceTool::DrawExplorer));
        CreateToolWindow(EditorWindow, Panel(&FCodeWorkspaceTool::DrawEditorArea), ImVec2(0.0f, 0.0f), true);
        CreateToolWindow(ProblemsWindow, Panel(&FCodeWorkspaceTool::DrawProblems));
        CreateToolWindow(SearchWindow, Panel(&FCodeWorkspaceTool::DrawSearch));
        CreateToolWindow(OutlineWindow, Panel(&FCodeWorkspaceTool::DrawOutline));

        ReloadsSeen = DotNet::GetFinishedScriptReloads();
        RestoreSession();
        ProblemsDueAt = ImGui::GetTime() + ProblemsAfterSaveSeconds;
    }

    void FCodeWorkspaceTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        for (TUniquePtr<FDirectoryWatcher>& Watcher : Watchers)
        {
            Watcher->Stop();
        }
        Watchers.Reset();
    }

    void FCodeWorkspaceTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
        ImGuiID Left = 0;
        ImGuiID Rest = 0;
        ImGuiID Right = 0;
        ImGuiID Center = 0;
        ImGuiID Bottom = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Left, 0.18f, &Left, &Rest);
        ImGui::DockBuilderSplitNode(Rest, ImGuiDir_Right, 0.2f, &Right, &Center);
        ImGui::DockBuilderSplitNode(Center, ImGuiDir_Down, 0.27f, &Bottom, &Center);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(ExplorerWindow), Left);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(EditorWindow), Center);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(OutlineWindow), Right);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(ProblemsWindow), Bottom);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(SearchWindow), Bottom);
    }

    bool FCodeWorkspaceTool::IsUnsavedDocument()
    {
        return Algo::AnyOf(Documents.begin(), Documents.end(), [](const TUniquePtr<FDocument>& Document) { return Document->bDirty; });
    }

    FCodeWorkspaceTool::FDocument* FCodeWorkspaceTool::FindDocument(FStringView VirtualPath)
    {
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            if (SamePath(Document->VirtualPath.View(), VirtualPath))
            {
                return Document.get();
            }
        }
        return nullptr;
    }

    FCodeWorkspaceTool::FDocument* FCodeWorkspaceTool::ActiveDocument()
    {
        return ActiveIndex >= 0 && ActiveIndex < Documents.Num() ? Documents[ActiveIndex].get() : nullptr;
    }

    FCodeWorkspaceTool::FDocument* FCodeWorkspaceTool::CreateDocument(FStringView VirtualPath)
    {
        if (!VFS::Exists(VirtualPath))
        {
            ImGuiX::Notifications::NotifyError("'{}' does not exist.", *FString(VirtualPath));
            return nullptr;
        }
        TUniquePtr<FDocument> Created = MakeUnique<FDocument>();
        FDocument& Document = *Created;
        Document.VirtualPath = VirtualPath;
        Document.DiskPath = *VFS::ResolvePath(VirtualPath);
        Document.Name = VFS::FileName(VirtualPath);

        if (EndsWithIgnoreCase(VirtualPath, ".cs"))
        {
            TUniquePtr<FCSharpScriptLanguage> Script = MakeUnique<FCSharpScriptLanguage>();
            Script->SetFilePath(Document.DiskPath.View());
            Document.CSharp = Script.get();
            Document.Language = Move(Script);
        }
        else if (EndsWithIgnoreCase(VirtualPath, ".slang") || EndsWithIgnoreCase(VirtualPath, ".hlsl"))
        {
            Document.Language = MakeUnique<FSlangLanguage>();
        }
        else if (EndsWithIgnoreCase(VirtualPath, ".rml") || EndsWithIgnoreCase(VirtualPath, ".rcss"))
        {
            Document.Language = MakeUnique<FRmlLanguage>(EndsWithIgnoreCase(VirtualPath, ".rcss"));
        }
        else if (EndsWithIgnoreCase(VirtualPath, ".json"))
        {
            Document.Language = MakeUnique<FPlainTextLanguage>("JSON");
        }
        else if (EndsWithIgnoreCase(VirtualPath, ".md"))
        {
            Document.Language = MakeUnique<FPlainTextLanguage>("Markdown");
        }
        else if (HasAnyExtension(VirtualPath, TSpan<const FStringView>({ ".py", ".yaml", ".yml", ".ini", ".cfg", ".sh" })))
        {
            Document.Language = MakeUnique<FPlainTextLanguage>("Text", "#");
        }
        else
        {
            Document.Language = MakeUnique<FPlainTextLanguage>("Text");
        }

        Document.Editor.SetLanguage(Document.Language.get());
        FDocument* Raw = &Document;
        Document.Editor.SetDefinitionHandler([this](const FCodeLocation& Location)
        {
            OpenDiskLocation(Location.Path.View(), { Location.Position, Location.Position });
        });
        Document.Editor.SetCommandHandler([this, Raw](FCodeEditor::ECommand Command, FCodePosition Position)
        {
            if (Command == FCodeEditor::ECommand::FindReferences)
            {
                FindReferences(*Raw, Position);
            }
            else
            {
                BeginRename(*Raw, Position);
            }
        });

        LoadDocument(Document);
        if (Document.CSharp != nullptr)
        {
            Document.CSharp->Warm(Document.Editor.GetDocument());
        }
        Documents.Add(Move(Created));
        return Raw;
    }

    void FCodeWorkspaceTool::OpenDocument(FStringView VirtualPath, FCodePosition Reveal, bool bRecordHistory)
    {
        FDocument* Document = FindDocument(VirtualPath);
        if (bRecordHistory)
        {
            PushHistory();
        }
        if (Document == nullptr)
        {
            Document = CreateDocument(VirtualPath);
            if (Document == nullptr)
            {
                return;
            }
        }
        for (int32 Index = 0; Index < Documents.Num(); ++Index)
        {
            if (Documents[Index].get() == Document)
            {
                SelectRequest = Index;
                ActiveIndex = Index;
            }
        }
        if (Reveal.Line >= 0)
        {
            Document->Editor.SetCursor(Reveal, true);
        }
        Document->Editor.Focus();
        ImGui::SetWindowFocus(*GetToolWindowName(EditorWindow));
        RememberSession();
    }

    void FCodeWorkspaceTool::LoadDocument(FDocument& Document)
    {
        FString Text;
        if (!VFS::ReadFile(Text, Document.VirtualPath.View()))
        {
            ImGuiX::Notifications::NotifyError("Could not read '{}'.", *Document.VirtualPath);
            return;
        }
        Text.ReplaceInline("\r", "");
        Document.SavedText = Text;
        Document.bDirty = false;
        if (Text != Document.Editor.GetText())
        {
            const FCodePosition Caret = Document.Editor.GetCursor();
            Document.Editor.SetText(Text.View());
            Document.Editor.SetCursor(Caret, false);
        }
        Document.CheckedVersion = Document.Editor.GetVersion();
    }

    bool FCodeWorkspaceTool::SaveDocument(FDocument& Document)
    {
        if (GetDefault<CCodeEditorSettings>()->bTrimTrailingWhitespaceOnSave)
        {
            Document.Editor.TrimTrailingWhitespace();
        }
        const FString Text = Document.Editor.GetText();
        if (!VFS::WriteFile(Document.VirtualPath.View(), Text.View()))
        {
            ImGuiX::Notifications::NotifyError("Failed to save '{}'.", *Document.VirtualPath);
            return false;
        }
        Document.SavedText = Text;
        Document.bDirty = false;
        if (Document.CSharp != nullptr)
        {
            ReloadsAtSave = DotNet::GetFinishedScriptReloads();
            DotNet::NotifyScriptSourceChanged(Document.DiskPath.View());
            ProblemsDueAt = ImGui::GetTime() + ProblemsAfterSaveSeconds;
        }
        return true;
    }

    void FCodeWorkspaceTool::OnSave()
    {
        if (FDocument* Document = ActiveDocument())
        {
            if (SaveDocument(*Document))
            {
                ImGuiX::Notifications::NotifyInfo("Saved '{}'.", *Document->Name);
            }
        }
    }

    bool FCodeWorkspaceTool::SaveFileChanges()
    {
        SaveAll();
        return !IsUnsavedDocument();
    }

    void FCodeWorkspaceTool::SaveAll()
    {
        int32 Saved = 0;
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            if (Document->bDirty && SaveDocument(*Document))
            {
                ++Saved;
            }
        }
        if (Saved > 0)
        {
            ImGuiX::Notifications::NotifyInfo("Saved {} files.", Saved);
        }
    }

    void FCodeWorkspaceTool::RequestClose(int32 Index)
    {
        if (Index < 0 || Index >= Documents.Num())
        {
            return;
        }
        PendingClose = Index;
        if (Documents[Index]->bDirty)
        {
            bOpenClosePrompt = true;
        }
    }

    void FCodeWorkspaceTool::CloseDocument(int32 Index)
    {
        if (Index < 0 || Index >= Documents.Num())
        {
            return;
        }
        Documents.RemoveAt(Index);
        if (ActiveIndex >= Documents.Num() || ActiveIndex > Index)
        {
            ActiveIndex = Math::Max(-1, ActiveIndex - 1);
        }
        ActiveIndex = Documents.IsEmpty() ? -1 : Math::Clamp(ActiveIndex, 0, Documents.Num() - 1);
        SelectRequest = ActiveIndex;
        RememberSession();
    }

    void FCodeWorkspaceTool::RememberSession() const
    {
        CEditorSessionSettings* Settings = GetMutableDefault<CEditorSessionSettings>();
        if (Settings == nullptr)
        {
            return;
        }
        Settings->CodeFiles.Reset();
        for (const TUniquePtr<FDocument>& Document : Documents)
        {
            Settings->CodeFiles.Add(Document->VirtualPath);
        }
        Settings->ActiveCodeFile = ActiveIndex >= 0 && ActiveIndex < Documents.Num() ? Documents[ActiveIndex]->VirtualPath : FString();
        GConfig->SaveSettings(CEditorSessionSettings::StaticClass());
    }

    void FCodeWorkspaceTool::RestoreSession()
    {
        const CEditorSessionSettings* Settings = GetDefault<CEditorSessionSettings>();
        if (Settings == nullptr)
        {
            return;
        }
        const TVector<FString> Files = Settings->CodeFiles;
        const FString Active = Settings->ActiveCodeFile;
        for (const FString& File : Files)
        {
            if (VFS::Exists(File.View()) && FindDocument(File.View()) == nullptr)
            {
                CreateDocument(File.View());
            }
        }
        for (int32 Index = 0; Index < Documents.Num(); ++Index)
        {
            if (SamePath(Documents[Index]->VirtualPath.View(), Active.View()) || ActiveIndex < 0)
            {
                ActiveIndex = Index;
            }
        }
        SelectRequest = ActiveIndex;
    }

    void FCodeWorkspaceTool::Update(const FUpdateContext& UpdateContext)
    {
        FEditorTool::Update(UpdateContext);
        if (bIndexDirty.exchange(false, Atomic::MemoryOrderAcquire))
        {
            RebuildIndex();
        }

        TVector<FString> Changed;
        {
            FScopeLock Lock(ChangedMutex);
            Changed = Move(ChangedPaths);
            ChangedPaths.Reset();
        }
        for (const FString& Path : Changed)
        {
            for (TUniquePtr<FDocument>& Document : Documents)
            {
                if (!SamePath(Document->DiskPath.View(), Path.View()))
                {
                    continue;
                }
                FString OnDisk;
                if (!VFS::ReadFile(OnDisk, Document->VirtualPath.View()))
                {
                    continue;
                }
                OnDisk.ReplaceInline("\r", "");
                if (OnDisk == Document->SavedText)
                {
                    continue;
                }
                if (Document->bDirty)
                {
                    ImGuiX::Notifications::NotifyWarning("'{}' changed on disk while it has unsaved edits here; keeping the edits.", *Document->Name);
                }
                else
                {
                    LoadDocument(*Document);
                }
            }
        }

        UpdateDocuments();

        const int32 Reloads = DotNet::GetFinishedScriptReloads();
        if (Reloads != ReloadsSeen)
        {
            ReloadsSeen = Reloads;
            ProblemsDueAt = ImGui::GetTime() + 0.25;
            if (ReloadsAtSave >= 0 && Reloads > ReloadsAtSave)
            {
                ReloadsAtSave = -1;
                if (DotNet::GetLastScriptReloadResult() == 0)
                {
                    ImGuiX::Notifications::NotifySuccess("Scripts reloaded.");
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Scripts did not compile, so the last build that worked is still running.");
                    bRevealProblems = true;
                }
            }
        }
        if (ProblemsDueAt > 0.0 && ImGui::GetTime() >= ProblemsDueAt)
        {
            RefreshProblems();
        }
    }

    void FCodeWorkspaceTool::UpdateDocuments()
    {
        constexpr double OutlineIntervalSeconds = 0.4;
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            if (Document->CheckedVersion != Document->Editor.GetVersion())
            {
                Document->CheckedVersion = Document->Editor.GetVersion();
                Document->bDirty = Document->Editor.GetText() != Document->SavedText;
            }
        }
        FDocument* Active = ActiveDocument();
        if (Active == nullptr)
        {
            return;
        }
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            Document->Editor.GetSettings().FontScale = Active->Editor.GetSettings().FontScale;
        }
        if (Active->OutlineVersion != Active->Editor.GetVersion() && ImGui::GetTime() >= Active->OutlineCheckedAt + OutlineIntervalSeconds)
        {
            Active->OutlineCheckedAt = ImGui::GetTime();
            TVector<FCodeSymbolInfo> Symbols;
            if (Active->Language->GetDocumentSymbols(Active->Editor.GetDocument(), Symbols))
            {
                Active->Outline = Move(Symbols);
                Active->OutlineVersion = Active->Editor.GetVersion();
            }
        }
    }

    void FCodeWorkspaceTool::RebuildIndex()
    {
        Files.Reset();
        for (int32 RootIndex = 0; RootIndex < Roots.Num(); ++RootIndex)
        {
            const FRoot& Root = Roots[RootIndex];
            VFS::RecursiveDirectoryIterator(Root.VirtualPath.View(), [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.IsHidden() || !HasAnyExtension(Info.Name.View(), TextExtensions))
                {
                    return;
                }
                const FString Relative = Info.VirtualPath.Mid(Math::Min(Root.VirtualPath.Len() + 1, Info.VirtualPath.Len()));
                if (IsInOutputFolder(Relative.View()))
                {
                    return;
                }
                FFileEntry& Entry = Files.EmplaceGetRef();
                Entry.VirtualPath = Info.VirtualPath;
                Entry.DiskPath = *VFS::ResolvePath(Info.VirtualPath.View());
                Entry.Name = Info.Name;
                Entry.Relative = Relative;
                Entry.Root = RootIndex;
            });
        }
        Algo::Sort(Files, [](const FFileEntry& A, const FFileEntry& B)
        {
            if (A.Root != B.Root)
            {
                return A.Root < B.Root;
            }
            return _stricmp(*A.Relative, *B.Relative) < 0;
        });

        TreeNodes.Reset();
        RootNodes.Reset();
        for (const FRoot& Root : Roots)
        {
            RootNodes.Add(TreeNodes.Num());
            TreeNodes.Add({ Root.Label, -1, {} });
        }
        for (int32 FileIndex = 0; FileIndex < Files.Num(); ++FileIndex)
        {
            const FFileEntry& Entry = Files[FileIndex];
            int32 Node = RootNodes[Entry.Root];
            FStringView Rest = Entry.Relative.View();
            for (int32 Slash = Rest.Find('/'); Slash != Constants::kIndexNone; Slash = Rest.Find('/'))
            {
                const FStringView Folder = Rest.Mid(0, Slash);
                int32 Child = -1;
                for (int32 Candidate : TreeNodes[Node].Children)
                {
                    if (TreeNodes[Candidate].File < 0 && TreeNodes[Candidate].Name.View() == Folder)
                    {
                        Child = Candidate;
                        break;
                    }
                }
                if (Child < 0)
                {
                    Child = TreeNodes.Num();
                    TreeNodes.Add({ FString(Folder), -1, {} });
                    TreeNodes[Node].Children.Add(Child);
                }
                Node = Child;
                Rest = Rest.Mid(Slash + 1);
            }
            const int32 Leaf = TreeNodes.Num();
            TreeNodes.Add({ Entry.Name, FileIndex, {} });
            TreeNodes[Node].Children.Add(Leaf);
        }
        for (FTreeNode& Node : TreeNodes)
        {
            Algo::Sort(Node.Children, [this](int32 A, int32 B)
            {
                const bool bFolderA = TreeNodes[A].File < 0;
                const bool bFolderB = TreeNodes[B].File < 0;
                return bFolderA != bFolderB ? bFolderA : _stricmp(*TreeNodes[A].Name, *TreeNodes[B].Name) < 0;
            });
        }

        const auto Script = Algo::FindIf(Files, [](const FFileEntry& Entry) { return EndsWithIgnoreCase(Entry.Name.View(), ".cs") && Entry.VirtualPath.View().StartsWith("/Game/"); });
        WorkspaceLanguage.SetFilePath(Script != Files.end() ? Script->DiskPath.View() : FStringView());
    }

    void FCodeWorkspaceTool::StartWatching()
    {
        for (const FRoot& Root : Roots)
        {
            TUniquePtr<FDirectoryWatcher>& Watcher = Watchers.EmplaceGetRef(MakeUnique<FDirectoryWatcher>());
            Watcher->Watch(FFixedString(*Root.DiskPath), [this, RootLength = Root.DiskPath.Len()](const FFileEvent& Event)
            {
                const FStringView Relative = Event.Path.View().Mid(Math::Min(RootLength + 1, Event.Path.Len()));
                if (IsInOutputFolder(Relative))
                {
                    return;
                }
                // Asset saves under Content are frequent and never change the text files listed here.
                const bool bText = HasAnyExtension(Event.Path.View(), TextExtensions);
                const bool bFolder = VFS::Extension(Event.Path.View()).IsEmpty();
                if (!bText && !bFolder)
                {
                    return;
                }
                if (Event.Action != EFileAction::Modified)
                {
                    bIndexDirty.store(true, Atomic::MemoryOrderRelease);
                }
                if (!bText)
                {
                    return;
                }
                FScopeLock Lock(ChangedMutex);
                ChangedPaths.Add(Event.Path);
            }, true);
        }
    }

    FString FCodeWorkspaceTool::VirtualPathOfDisk(FStringView DiskPath) const
    {
        for (const FFileEntry& Entry : Files)
        {
            if (SamePath(Entry.DiskPath.View(), DiskPath))
            {
                return Entry.VirtualPath;
            }
        }
        const FFixedString Resolved = VFS::ResolveToVirtualPath(DiskPath);
        return FString(*Resolved);
    }

    void FCodeWorkspaceTool::PushHistory()
    {
        FDocument* Active = ActiveDocument();
        if (Active == nullptr)
        {
            return;
        }
        const FLocation Here{ Active->VirtualPath, Active->Editor.GetCursor() };
        if (!BackHistory.IsEmpty() && SamePath(BackHistory.Last().VirtualPath.View(), Here.VirtualPath.View()) && BackHistory.Last().Position.Line == Here.Position.Line)
        {
            return;
        }
        BackHistory.Add(Here);
        if (BackHistory.Num() > MaxHistory)
        {
            BackHistory.RemoveAt(0);
        }
        ForwardHistory.Reset();
    }

    void FCodeWorkspaceTool::Navigate(const FLocation& Location, bool bRecordHistory)
    {
        OpenDocument(Location.VirtualPath.View(), Location.Position, bRecordHistory);
    }

    void FCodeWorkspaceTool::GoBack()
    {
        if (BackHistory.IsEmpty())
        {
            return;
        }
        if (FDocument* Active = ActiveDocument())
        {
            ForwardHistory.Emplace(Active->VirtualPath, Active->Editor.GetCursor());
        }
        const FLocation Target = BackHistory.Last();
        BackHistory.Pop();
        Navigate(Target, false);
    }

    void FCodeWorkspaceTool::GoForward()
    {
        if (ForwardHistory.IsEmpty())
        {
            return;
        }
        if (FDocument* Active = ActiveDocument())
        {
            BackHistory.Emplace(Active->VirtualPath, Active->Editor.GetCursor());
        }
        const FLocation Target = ForwardHistory.Last();
        ForwardHistory.Pop();
        Navigate(Target, false);
    }

    void FCodeWorkspaceTool::OpenDiskLocation(FStringView DiskPath, FCodeRange Range)
    {
        const FString Target = VirtualPathOfDisk(DiskPath);
        if (Target.IsEmpty())
        {
            ImGuiX::Notifications::NotifyWarning("'{}' is outside the project's folders.", *FString(DiskPath));
            return;
        }
        OpenRange(Target.View(), Range);
    }

    void FCodeWorkspaceTool::OpenRange(FStringView VirtualPath, FCodeRange Range)
    {
        OpenDocument(VirtualPath, Range.Start, true);
        if (FDocument* Document = FindDocument(VirtualPath); Document != nullptr && Range.End != Range.Start)
        {
            Document->Editor.Select(Range, true);
        }
    }

    void FCodeWorkspaceTool::SyncScripts()
    {
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            if (Document->CSharp != nullptr && Document->bDirty)
            {
                Document->CSharp->Sync(Document->Editor.GetDocument());
            }
        }
    }

    FCSharpScriptLanguage* FCodeWorkspaceTool::AnyScriptLanguage()
    {
        if (FDocument* Active = ActiveDocument(); Active != nullptr && Active->CSharp != nullptr)
        {
            return Active->CSharp;
        }
        for (TUniquePtr<FDocument>& Document : Documents)
        {
            if (Document->CSharp != nullptr)
            {
                return Document->CSharp;
            }
        }
        return WorkspaceLanguage.GetFilePath().IsEmpty() ? nullptr : &WorkspaceLanguage;
    }

    void FCodeWorkspaceTool::FindReferences(FDocument& Document, FCodePosition Position)
    {
        if (Document.CSharp == nullptr)
        {
            ImGuiX::Notifications::NotifyInfo("Find references needs a C# script.");
            return;
        }
        SyncScripts();
        TVector<FCSharpScriptLanguage::FReference> References;
        if (Document.CSharp->FindReferences(Document.Editor.GetDocument(), Position, References) == FCSharpScriptLanguage::EReply::Busy)
        {
            ImGuiX::Notifications::NotifyInfo("Still reading the scripts, try again in a moment.");
            return;
        }
        const FCodeRange Word = CodeLanguageUtil::WordRangeAt(Document.Editor.GetDocument(), *Document.Language, Position);
        const FString Name = Document.Editor.GetDocument().GetRangeText(Word);
        Results.Reset();
        for (const FCSharpScriptLanguage::FReference& Reference : References)
        {
            const int32 At = Reference.Preview.Find(*Name);
            Results.Emplace(VirtualPathOfDisk(Reference.Path.View()), Reference.Path, Reference.Range, Reference.Preview, Reference.bDeclaration, At == Constants::kIndexNone ? -1 : (int32)At, Name.Len());
        }
        Algo::Sort(Results, [](const FSearchHit& A, const FSearchHit& B)
        {
            const int32 Order = _stricmp(*A.VirtualPath, *B.VirtualPath);
            return Order != 0 ? Order < 0 : A.Range.Start < B.Range.Start;
        });
        ResultsTitle = Results.IsEmpty() ? Lumina::Format("No references to '{}'", Name) : Lumina::Format("{} references to '{}'", Results.Num(), Name);
        bShowingReferences = true;
        bRevealSearch = true;
    }

    void FCodeWorkspaceTool::BeginRename(FDocument& Document, FCodePosition Position)
    {
        if (Document.CSharp == nullptr)
        {
            ImGuiX::Notifications::NotifyInfo("Rename needs a C# script.");
            return;
        }
        SyncScripts();
        TVector<FCSharpScriptLanguage::FReference> References;
        if (Document.CSharp->FindReferences(Document.Editor.GetDocument(), Position, References) == FCSharpScriptLanguage::EReply::Busy)
        {
            ImGuiX::Notifications::NotifyInfo("Still reading the scripts, try again in a moment.");
            return;
        }
        if (References.IsEmpty())
        {
            ImGuiX::Notifications::NotifyInfo("Nothing here to rename.");
            return;
        }
        RenameTargets.Reset();
        for (const FCSharpScriptLanguage::FReference& Reference : References)
        {
            RenameTargets.Emplace(VirtualPathOfDisk(Reference.Path.View()), Reference.Path, Reference.Range, Reference.Preview, Reference.bDeclaration);
        }
        const FCSharpScriptLanguage::FReference& First = References[0];
        const FCodeRange Word = CodeLanguageUtil::WordRangeAt(Document.Editor.GetDocument(), *Document.Language, Position);
        RenameFrom = Document.Editor.GetDocument().GetRangeText(Word);
        if (RenameFrom.IsEmpty())
        {
            RenameFrom = First.Preview;
        }
        RenameTo = RenameFrom;
        bRenameAppearing = true;
    }

    void FCodeWorkspaceTool::ApplyRename()
    {
        const FStringView NewName = RenameTo.View();
        if (!IsIdentifier(NewName) || NewName == RenameFrom.View())
        {
            return;
        }
        THashMap<FString, TVector<FCodeTextEdit>> ByFile;
        for (const FSearchHit& Target : RenameTargets)
        {
            ByFile[Target.DiskPath].Emplace(FString(), Target.Range, FString(NewName));
        }
        int32 Places = 0;
        for (auto& [DiskPath, Edits] : ByFile)
        {
            Places += Edits.Num();
            const FString Virtual = VirtualPathOfDisk(DiskPath.View());
            if (FDocument* Open = FindDocument(Virtual.View()))
            {
                Open->Editor.ApplyEdits(Edits);
            }
            else
            {
                ApplyEditsToFile(DiskPath.View(), Move(Edits));
            }
        }
        ImGuiX::Notifications::NotifySuccess("Renamed '{}' to '{}' in {} places across {} files.", *RenameFrom, *FString(NewName), Places, ByFile.Num());
        RenameTargets.Reset();
        SyncScripts();
        ProblemsDueAt = ImGui::GetTime() + ProblemsAfterSaveSeconds;
    }

    void FCodeWorkspaceTool::ApplyEditsToFile(FStringView DiskPath, TVector<FCodeTextEdit> Edits)
    {
        const FString Virtual = VirtualPathOfDisk(DiskPath);
        FString Text;
        if (Virtual.IsEmpty() || !VFS::ReadFile(Text, Virtual.View()))
        {
            ImGuiX::Notifications::NotifyError("Could not edit '{}'.", *FString(DiskPath));
            return;
        }
        const bool bCarriageReturns = Text.Find('\r') != Constants::kIndexNone;
        Text.ReplaceInline("\r", "");
        FCodeDocument Document;
        Document.SetText(Text.View());
        Algo::Sort(Edits, [](const FCodeTextEdit& A, const FCodeTextEdit& B) { return B.Range.Start < A.Range.Start; });
        for (const FCodeTextEdit& Edit : Edits)
        {
            Document.Erase(Edit.Range);
            Document.Insert(Edit.Range.Start, Edit.Text.View());
        }
        FString Result = Document.GetText();
        if (bCarriageReturns)
        {
            FString WithReturns;
            WithReturns.Reserve(Result.Len() + Result.Len() / 32);
            for (const char C : Result)
            {
                if (C == '\n')
                {
                    WithReturns.AppendChar('\r');
                }
                WithReturns.AppendChar(C);
            }
            Result = Move(WithReturns);
        }
        if (VFS::WriteFile(Virtual.View(), Result.View()) && EndsWithIgnoreCase(DiskPath, ".cs"))
        {
            DotNet::NotifyScriptSourceChanged(DiskPath);
        }
    }

    void FCodeWorkspaceTool::RefreshProblems()
    {
        FCSharpScriptLanguage* Script = AnyScriptLanguage();
        if (Script == nullptr)
        {
            ProblemsDueAt = 0.0;
            return;
        }
        SyncScripts();
        TVector<FCSharpScriptLanguage::FProjectDiagnostic> Found;
        const FCSharpScriptLanguage::EReply Reply = Script->GetProjectDiagnostics(Found);
        if (Reply == FCSharpScriptLanguage::EReply::Busy)
        {
            ProblemsDueAt = ImGui::GetTime() + ProblemsRetrySeconds;
            return;
        }
        ProblemsDueAt = 0.0;
        ScriptProblems.Reset();
        for (FCSharpScriptLanguage::FProjectDiagnostic& Problem : Found)
        {
            ScriptProblems.Emplace(VirtualPathOfDisk(Problem.Path.View()), Move(Problem.Diagnostic));
        }
    }

    void FCodeWorkspaceTool::RunSearch()
    {
        Results.Reset();
        bShowingReferences = false;
        const FStringView Query(SearchQuery);
        if (Query.IsEmpty())
        {
            ResultsTitle.Reset();
            return;
        }

        TVector<FString> Includes;
        for (FStringView Rest(SearchInclude); !Rest.IsEmpty();)
        {
            const int32 Comma = Rest.FindFirstOf(", ");
            FStringView Part = Comma == Constants::kIndexNone ? Rest : Rest.Left(Comma);
            while (!Part.IsEmpty() && Part[0] == '*')
            {
                Part.RemovePrefix(1);
            }
            if (!Part.IsEmpty())
            {
                Includes.Emplace(Part);
            }
            Rest = Comma == Constants::kIndexNone ? FStringView() : Rest.Mid(Comma + 1);
        }

        auto IsWord = [](char C) { return CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_'; };
        int32 FileCount = 0;
        for (const FFileEntry& Entry : Files)
        {
            if (!Includes.IsEmpty() && !Algo::AnyOf(Includes.begin(), Includes.end(), [&](const FString& Include) { return CodeText::FindIgnoreCase(Entry.Relative.View(), Include.View(), 0) >= 0; }))
            {
                continue;
            }
            FString Text;
            if (FDocument* Open = FindDocument(Entry.VirtualPath.View()))
            {
                Text = Open->Editor.GetText();
            }
            else if (!VFS::ReadFile(Text, Entry.VirtualPath.View()))
            {
                continue;
            }
            bool bAny = false;
            int32 Line = 0;
            int32 LineStart = 0;
            while (LineStart <= Text.Len() && Results.Num() < MaxSearchResults)
            {
                int32 LineEnd = Text.Find('\n', LineStart);
                LineEnd = LineEnd == Constants::kIndexNone ? Text.Len() : LineEnd;
                FStringView LineText(Text.GetData() + LineStart, LineEnd - LineStart);
                if (!LineText.IsEmpty() && LineText.Last() == '\r')
                {
                    LineText.RemoveSuffix(1);
                }
                for (int32 From = 0;;)
                {
                    int32 Found;
                    if (bSearchCase)
                    {
                        const int32 At = LineText.Find(Query, (size_t)From);
                        Found = At == Constants::kIndexNone ? -1 : (int32)At;
                    }
                    else
                    {
                        Found = CodeText::FindIgnoreCase(LineText, Query, From);
                    }
                    if (Found < 0)
                    {
                        break;
                    }
                    const int32 End = Found + Query.Len();
                    From = End;
                    if (bSearchWord && ((Found > 0 && IsWord(LineText[Found - 1])) || (End < LineText.Len() && IsWord(LineText[End]))))
                    {
                        continue;
                    }
                    const int32 Indent = CodeText::LeadingWhitespace(LineText);
                    Results.Add({ Entry.VirtualPath, Entry.DiskPath, { { Line, Found }, { Line, End } }, Trimmed(LineText), false, Found - Indent, End - Found });
                    bAny = true;
                }
                ++Line;
                LineStart = LineEnd + 1;
            }
            FileCount += bAny ? 1 : 0;
        }
        ResultsTitle = Lumina::Format("{} results in {} files{}", Results.Num(), FileCount, Results.Num() >= MaxSearchResults ? ", stopped at the limit" : "");
    }

    void FCodeWorkspaceTool::ReplaceAllInFiles()
    {
        THashMap<FString, TVector<FCodeTextEdit>> ByFile;
        for (const FSearchHit& Hit : Results)
        {
            ByFile[Hit.VirtualPath].Emplace(FString(), Hit.Range, FString(SearchReplace));
        }
        int32 Count = 0;
        for (auto& [VirtualPath, Edits] : ByFile)
        {
            Count += Edits.Num();
            if (FDocument* Open = FindDocument(VirtualPath.View()))
            {
                Open->Editor.ApplyEdits(Edits);
            }
            else
            {
                ApplyEditsToFile(FString(*VFS::ResolvePath(VirtualPath.View())).View(), Move(Edits));
            }
        }
        ImGuiX::Notifications::NotifySuccess("Replaced {} matches in {} files.", Count, ByFile.Num());
        RunSearch();
    }

    void FCodeWorkspaceTool::HandleShortcuts()
    {
        // Polled rather than routed, so they work from every panel of the workspace.
        const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
        auto Pressed = [](const SShortcut& Shortcut) { return Shortcut.IsPressed(0, ImGuiInputFlags_RouteAlways); };
        if (Pressed(Keys.GoToFile))
        {
            OpenPicker(EPicker::Files);
        }
        else if (Pressed(Keys.GoToLine))
        {
            OpenPicker(EPicker::Line);
        }
        else if (Pressed(Keys.GoToSymbol))
        {
            OpenPicker(EPicker::WorkspaceSymbols);
        }
        else if (Pressed(Keys.GoToSymbolInFile))
        {
            OpenPicker(EPicker::DocumentSymbols);
        }
        else if (Pressed(Keys.FindInFiles))
        {
            if (FDocument* Active = ActiveDocument(); Active != nullptr && Active->Editor.HasSelection())
            {
                const FString Selected = Active->Editor.GetSelectedText();
                if (Selected.Find('\n') == Constants::kIndexNone)
                {
                    std::snprintf(SearchQuery, sizeof(SearchQuery), "%s", *Selected);
                }
            }
            bFocusSearch = true;
            ImGui::SetWindowFocus(*GetToolWindowName(SearchWindow));
        }
        else if (Pressed(Keys.SaveAll))
        {
            SaveAll();
        }
        else if (Pressed(Keys.CloseFile))
        {
            RequestClose(ActiveIndex);
        }
        else if (const int32 Step = Pressed(Keys.NextFile) ? 1 : Pressed(Keys.PreviousFile) ? -1 : 0; Step != 0 && !Documents.IsEmpty())
        {
            const int32 Count = Documents.Num();
            SelectRequest = ((ActiveIndex + Step) % Count + Count) % Count;
        }
        else if (Pressed(Keys.NavigateBack))
        {
            GoBack();
        }
        else if (Pressed(Keys.NavigateForward))
        {
            GoForward();
        }
        else if (Pressed(Keys.ReloadScripts))
        {
            DotNet::RequestScriptReload();
        }
    }

    void FCodeWorkspaceTool::DrawHelpMenu()
    {
        if (ImGui::BeginMenu(LE_ICON_KEYBOARD " Keyboard Shortcuts"))
        {
            CodeEditorPreferences::DrawShortcutReference();
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(LE_ICON_PENCIL " Change Shortcuts"))
        {
            ToolContext->OpenSettings(CCodeEditorShortcuts::StaticClass());
        }
    }

    void FCodeWorkspaceTool::OpenPicker(EPicker Kind)
    {
        if ((Kind == EPicker::DocumentSymbols || Kind == EPicker::Line) && ActiveDocument() == nullptr)
        {
            return;
        }
        Picker = Kind;
        bPickerAppearing = true;
        Memory::Memzero(PickerQuery, sizeof(PickerQuery));
        PickerQueryUsed.Reset();
        PickerSelected = 0;
        PickerSymbols.Reset();
    }

    void FCodeWorkspaceTool::DrawExplorer()
    {
        const float ButtonWidth = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##filter", LE_ICON_MAGNIFY " Filter files", ExplorerFilter, sizeof(ExplorerFilter));
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH, ImVec2(ButtonWidth, 0.0f)))
        {
            bIndexDirty.store(true, Atomic::MemoryOrderRelease);
        }
        ImGui::SetItemTooltip("Read the folders again.");
        ImGui::Separator();

        ImGui::BeginChild("##files");
        const FStringView Filter(ExplorerFilter);
        if (Roots.IsEmpty())
        {
            ImGui::TextDisabled("This project has no script, content or shader folders.");
        }
        else if (Filter.IsEmpty())
        {
            for (int32 RootIndex = 0; RootIndex < RootNodes.Num(); ++RootIndex)
            {
                DrawTree(RootNodes[RootIndex], Roots[RootIndex].VirtualPath);
            }
        }
        else
        {
            TVector<FPickerRow> Matches;
            for (const FFileEntry& Entry : Files)
            {
                const int32 Score = Math::Max(FuzzyScore(Filter, Entry.Name.View()) * 2, FuzzyScore(Filter, Entry.Relative.View()));
                if (Score > 0)
                {
                    Matches.Emplace(Score, FileIcon(Entry.Name.View()), Entry.Name, Roots[Entry.Root].Label + "/" + Entry.Relative, Entry.VirtualPath);
                }
            }
            Algo::StableSort(Matches, [](const FPickerRow& A, const FPickerRow& B) { return A.Score > B.Score; });
            for (int32 Index = 0; Index < Math::Min(Matches.Num(), MaxPickerRows); ++Index)
            {
                const FPickerRow& Match = Matches[Index];
                ImGui::PushID(Index);
                if (ImGui::Selectable(*Lumina::Format("{} {}", Match.Icon, Match.Label)))
                {
                    OpenDocument(Match.VirtualPath.View());
                }
                ImGui::SetItemTooltip("%s", *Match.Detail);
                ImGui::PopID();
            }
            if (Matches.IsEmpty())
            {
                ImGui::TextDisabled("No file matches.");
            }
        }
        ImGui::EndChild();
        DrawNewFilePopup();
    }

    void FCodeWorkspaceTool::DrawTree(int32 NodeIndex, const FString& Folder)
    {
        const FTreeNode& Node = TreeNodes[NodeIndex];
        if (Node.File >= 0)
        {
            const FFileEntry& Entry = Files[Node.File];
            const FDocument* Active = ActiveDocument();
            const FDocument* Open = FindDocument(Entry.VirtualPath.View());
            const bool bActive = Active != nullptr && Active == Open;
            const ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | (bActive ? ImGuiTreeNodeFlags_Selected : 0);
            ImGui::TreeNodeEx(*Entry.Name, Flags, "%s %s%s", FileIcon(Entry.Name.View()), *Entry.Name, Open != nullptr && Open->bDirty ? " *" : "");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                OpenDocument(Entry.VirtualPath.View());
            }
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem(LE_ICON_FILE_DOCUMENT_OUTLINE " Open"))
                {
                    OpenDocument(Entry.VirtualPath.View());
                }
                if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Path"))
                {
                    ImGui::SetClipboardText(*Entry.VirtualPath);
                }
                if (ImGui::MenuItem(LE_ICON_OPEN_IN_NEW " Open Externally"))
                {
                    Platform::LaunchURL(UTF8_TO_TCHAR(*Entry.DiskPath));
                }
                if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Show in Folder"))
                {
                    Platform::LaunchURL(UTF8_TO_TCHAR(*FString(VFS::Parent(Entry.DiskPath.View(), true))));
                }
                ImGui::EndPopup();
            }
            return;
        }

        const bool bRoot = Algo::Contains(RootNodes, NodeIndex);
        const ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth | (bRoot ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        const bool bOpen = ImGui::TreeNodeEx(*Node.Name, Flags, "%s %s", bRoot ? LE_ICON_FOLDER : LE_ICON_FOLDER_OUTLINE, *Node.Name);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem(LE_ICON_FILE_PLUS_OUTLINE " New File..."))
            {
                NewFileFolder = Folder;
                Memory::Memzero(NewFileBuffer, sizeof(NewFileBuffer));
                bNewFileAppearing = true;
            }
            if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Path"))
            {
                ImGui::SetClipboardText(*Folder);
            }
            if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Show in Folder"))
            {
                Platform::LaunchURL(UTF8_TO_TCHAR(*VFS::ResolvePath(Folder.View())));
            }
            ImGui::EndPopup();
        }
        if (bOpen)
        {
            for (const int32 Child : Node.Children)
            {
                const FTreeNode& ChildNode = TreeNodes[Child];
                DrawTree(Child, ChildNode.File >= 0 ? Folder : Folder + "/" + ChildNode.Name);
            }
            ImGui::TreePop();
        }
    }

    void FCodeWorkspaceTool::DrawNewFilePopup()
    {
        constexpr const char* PopupName = "##CodeNewFile";
        if (bNewFileAppearing)
        {
            ImGui::OpenPopup(PopupName);
        }
        if (!ImGui::BeginPopup(PopupName))
        {
            return;
        }
        const bool bAppearing = bNewFileAppearing;
        bNewFileAppearing = false;

        ImGui::TextColored(EditorColors::SectionHeader(), "New file in %s", *NewFileFolder);
        if (bAppearing)
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(320.0f);
        const bool bSubmit = ImGui::InputTextWithHint("##name", "Name.cs", NewFileBuffer, sizeof(NewFileBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        const FStringView Name(NewFileBuffer);
        const FString Path = NewFileFolder + "/" + FString(Name);
        const FStringView Stem = VFS::FileName(Path.View(), true);
        FString Reason;
        if (AssetOps::IsValidLeafName(Name, &Reason) && Stem.IsEmpty())
        {
            Reason = "A file needs a name before its extension.";
        }
        const bool bKnown = HasAnyExtension(Name, TextExtensions);
        const bool bExists = Reason.IsEmpty() && bKnown && VFS::Exists(Path.View());
        const bool bValid = Reason.IsEmpty() && bKnown && !bExists;
        if (!Name.IsEmpty() && !Reason.IsEmpty())
        {
            ImGui::TextColored(EditorColors::Danger(), "%s", *Reason);
        }
        else if (!Name.IsEmpty() && !bKnown)
        {
            ImGui::TextColored(EditorColors::Warning(), "Use a text extension such as .cs, .slang, .rml or .json.");
        }
        if (bExists)
        {
            ImGui::TextColored(EditorColors::Danger(), "A file with that name already exists.");
        }

        ImGui::BeginDisabled(!bValid);
        if (ImGui::Button(LE_ICON_FILE_PLUS_OUTLINE " Create") || (bSubmit && bValid))
        {
            const bool bScript = EndsWithIgnoreCase(Name, ".cs");
            const FString Source = bScript ? CSharpScript::NewScriptSource(Stem) : FString();
            if (VFS::WriteFile(Path.View(), Source.View()))
            {
                ImGui::CloseCurrentPopup();
                RebuildIndex();
                OpenDocument(Path.View());
                if (bScript)
                {
                    DotNet::NotifyScriptSourceChanged(FString(*VFS::ResolvePath(Path.View())).View());
                }
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Could not create '{}'.", *Path);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || GetDefault<CCodeEditorShortcuts>()->Dismiss.IsPressed(0, ImGuiInputFlags_RouteAlways))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void FCodeWorkspaceTool::DrawToolbar()
    {
        FDocument* Active = ActiveDocument();
        auto ToolbarButton = [](const char* Label, bool bEnabled, const char* Tooltip)
        {
            ImGui::BeginDisabled(!bEnabled);
            const bool bPressed = ImGui::Button(Label);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", Tooltip);
            ImGui::SameLine();
            return bPressed;
        };
        auto Divider = []()
        {
            ImGui::TextDisabled("|");
            ImGui::SameLine();
        };
        const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
        auto Hint = [](const char* Text, const SShortcut& Shortcut)
        {
            const FString Label = Shortcut.GetLabel();
            return Label.IsEmpty() ? Lumina::Format("{}.", Text) : Lumina::Format("{} ({}).", Text, Label);
        };

        if (ToolbarButton(LE_ICON_ARROW_LEFT, !BackHistory.IsEmpty(), *Hint("Back", Keys.NavigateBack)))
        {
            GoBack();
        }
        if (ToolbarButton(LE_ICON_ARROW_RIGHT, !ForwardHistory.IsEmpty(), *Hint("Forward", Keys.NavigateForward)))
        {
            GoForward();
        }
        Divider();
        if (ToolbarButton(LE_ICON_CONTENT_SAVE " Save", Active != nullptr && Active->bDirty, "Save this file (Ctrl+S). Saving a script reloads the scripts."))
        {
            OnSave();
        }
        if (ToolbarButton(LE_ICON_CONTENT_SAVE_ALL " Save All", IsUnsavedDocument(), *Hint("Save every changed file", Keys.SaveAll)))
        {
            SaveAll();
        }
        Divider();
        if (ToolbarButton(LE_ICON_FILE_FIND_OUTLINE " Go to File", !Files.IsEmpty(), *Lumina::Format("{} {}", Hint("Open a file by name", Keys.GoToFile), Hint("Go to a line", Keys.GoToLine))))
        {
            OpenPicker(EPicker::Files);
        }
        if (ToolbarButton(LE_ICON_FUNCTION " Go to Symbol", true, *Lumina::Format("{} {}", Hint("Find a type or member in the scripts", Keys.GoToSymbol), Hint("Find one in this file", Keys.GoToSymbolInFile))))
        {
            OpenPicker(EPicker::WorkspaceSymbols);
        }
        if (ToolbarButton(LE_ICON_MAGNIFY " Find in Files", true, *Hint("Search every file in the project", Keys.FindInFiles)))
        {
            bFocusSearch = true;
            ImGui::SetWindowFocus(*GetToolWindowName(SearchWindow));
        }
        Divider();
        if (ToolbarButton(LE_ICON_FORMAT_ALIGN_LEFT " Format", Active != nullptr, *Hint("Re-indent this file", Keys.FormatDocument)))
        {
            Active->Editor.FormatDocument();
        }
        if (ToolbarButton(LE_ICON_CODE_BRACES " View", true, "Theme and display options for every code editor, and the keyboard shortcuts."))
        {
            ImGui::OpenPopup("##CodeView");
        }
        if (ToolbarButton(LE_ICON_REFRESH " Reload Scripts", true, *Hint("Recompile and reload every script now", Keys.ReloadScripts)))
        {
            DotNet::RequestScriptReload();
        }
        if (ToolbarButton(LE_ICON_OPEN_IN_NEW, Active != nullptr, "Open this file in the application the system uses for it."))
        {
            Platform::LaunchURL(UTF8_TO_TCHAR(*Active->DiskPath));
        }
        ImGui::NewLine();

        if (ImGui::BeginPopup("##CodeView"))
        {
            CodeEditorPreferences::DrawQuickSettings(ToolContext);
            ImGui::EndPopup();
        }
    }

    void FCodeWorkspaceTool::DrawEditorArea()
    {
        DrawToolbar();
        ImGui::Separator();

        if (Documents.IsEmpty())
        {
            const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
            const TPair<const SShortcut*, const char*> Hints[] =
            {
                { &Keys.GoToFile, "Go to a file" },
                { &Keys.GoToSymbol, "Go to a type or member in the scripts" },
                { &Keys.FindInFiles, "Find and replace in every file" },
                { &Keys.ReloadScripts, "Reload the scripts" },
                { &Keys.GoToDefinition, "Go to a definition" },
            };
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
            ImGui::TextColored(EditorColors::TextDim(), "Open a file from the explorer, or");
            for (const auto& [Shortcut, Text] : Hints)
            {
                if (Shortcut->Primary.IsValid())
                {
                    ImGui::TextColored(EditorColors::SectionHeader(), "%-20s", *Shortcut->GetLabel());
                    ImGui::SameLine();
                    ImGui::TextUnformatted(Text);
                }
            }
        }
        else if (ImGui::BeginTabBar("##CodeTabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
        {
            int32 CloseOthersOf = -1;
            for (int32 Index = 0; Index < Documents.Num(); ++Index)
            {
                FDocument& Document = *Documents[Index];
                ImGuiTabItemFlags Flags = Document.bDirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
                if (SelectRequest == Index)
                {
                    Flags |= ImGuiTabItemFlags_SetSelected;
                }
                const FString Label = Lumina::Format("{} {}###{}", FileIcon(Document.Name.View()), Document.Name, Document.VirtualPath);
                bool bOpen = true;
                const bool bSelected = ImGui::BeginTabItem(*Label, &bOpen, Flags);
                ImGui::SetItemTooltip("%s", *Document.VirtualPath);
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Close", *GetDefault<CCodeEditorShortcuts>()->CloseFile.GetLabel()))
                    {
                        RequestClose(Index);
                    }
                    if (ImGui::MenuItem("Close Saved Others"))
                    {
                        CloseOthersOf = Index;
                    }
                    if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Path"))
                    {
                        ImGui::SetClipboardText(*Document.VirtualPath);
                    }
                    if (ImGui::MenuItem(LE_ICON_OPEN_IN_NEW " Open Externally"))
                    {
                        Platform::LaunchURL(UTF8_TO_TCHAR(*Document.DiskPath));
                    }
                    ImGui::EndPopup();
                }
                if (bSelected)
                {
                    if (SelectRequest < 0 || SelectRequest == Index)
                    {
                        SelectRequest = -1;
                        if (ActiveIndex != Index)
                        {
                            ActiveIndex = Index;
                            RememberSession();
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (!bOpen)
                {
                    RequestClose(Index);
                }
            }
            ImGui::EndTabBar();

            if (CloseOthersOf >= 0)
            {
                const FDocument* Keep = Documents[CloseOthersOf].get();
                for (int32 Index = Documents.Num() - 1; Index >= 0; --Index)
                {
                    if (Documents[Index].get() != Keep && !Documents[Index]->bDirty)
                    {
                        CloseDocument(Index);
                    }
                }
                for (int32 Index = 0; Index < Documents.Num(); ++Index)
                {
                    ActiveIndex = Documents[Index].get() == Keep ? Index : ActiveIndex;
                }
                SelectRequest = ActiveIndex;
                RememberSession();
            }
        }

        if (FDocument* Active = ActiveDocument())
        {
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const float StatusHeight = ImGui::GetFrameHeightWithSpacing();
            ImGui::PushID(*Active->VirtualPath);
            Active->Editor.Render("##code", ImVec2(Avail.x, Math::Max(32.0f, Avail.y - StatusHeight)));
            ImGui::PopID();
            DrawStatusBar(*Active);
        }

        if (bRevealProblems)
        {
            bRevealProblems = false;
            RevealDockedWindow(*GetToolWindowName(ProblemsWindow));
        }
        if (bRevealSearch)
        {
            bRevealSearch = false;
            RevealDockedWindow(*GetToolWindowName(SearchWindow));
        }
        DrawPicker();
        DrawRenamePopup();
        DrawClosePrompt();
    }

    void FCodeWorkspaceTool::DrawStatusBar(FDocument& Document)
    {
        const FCodePosition Cursor = Document.Editor.GetCursor();
        const int32 Column = CodeText::VisualColumn(Document.Editor.GetDocument().GetLineView(Cursor.Line), Cursor.Column, Document.Editor.GetSettings().TabSize);
        constexpr float Gap = 16.0f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::TextDim(), "%s", *Document.VirtualPath);
        if (const FCodeSymbolInfo* Symbol = EnclosingSymbol(Document.Outline, Cursor.Line))
        {
            ImGui::SameLine(0.0f, Gap * 0.5f);
            ImGui::TextColored(EditorColors::TextDim(), "%s %s%s%s", SymbolIcon(Symbol->Kind), *Symbol->Container, Symbol->Container.IsEmpty() ? "" : ".", *Symbol->Name);
        }
        ImGui::SameLine(0.0f, Gap);
        ImGui::TextColored(EditorColors::TextDim(), "Ln %d, Col %d", Cursor.Line + 1, Column + 1);
        ImGui::SameLine(0.0f, Gap);
        ImGui::TextColored(EditorColors::TextDim(), "%d lines", Document.Editor.GetLineCount());
        ImGui::SameLine(0.0f, Gap);
        const FStringView LanguageName = Document.Language->GetName();
        ImGui::TextColored(Document.CSharp != nullptr ? EditorColors::Success() : EditorColors::TextDim(), "%.*s", LanguageName.Len(), LanguageName.GetData());
        if (ReloadsAtSave >= 0)
        {
            ImGui::SameLine(0.0f, Gap);
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_REFRESH " Reloading scripts");
        }

        int32 Errors = 0;
        int32 Warnings = 0;
        const FCodeDiagnostic* First = nullptr;
        for (const FCodeDiagnostic& Diagnostic : Document.Editor.GetDiagnostics())
        {
            Errors += Diagnostic.Severity == ECodeSeverity::Error ? 1 : 0;
            Warnings += Diagnostic.Severity == ECodeSeverity::Warning ? 1 : 0;
            First = First == nullptr && Diagnostic.Severity != ECodeSeverity::Info ? &Diagnostic : First;
        }
        ImGui::SameLine(0.0f, Gap);
        if (Errors == 0 && Warnings == 0)
        {
            ImGui::TextColored(EditorColors::Success(), LE_ICON_CHECK_CIRCLE_OUTLINE " No problems");
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Errors > 0 ? EditorColors::Danger() : EditorColors::Warning());
        const FString Label = Lumina::Format(LE_ICON_ALERT_CIRCLE_OUTLINE " {} errors, {} warnings", Errors, Warnings);
        if (ImGui::SmallButton(*Label) && First != nullptr)
        {
            Document.Editor.Select(First->Range);
            Document.Editor.Focus();
        }
        ImGui::PopStyleColor();
        if (First != nullptr)
        {
            ImGui::SetItemTooltip("%s", *First->Message);
        }
    }

    void FCodeWorkspaceTool::DrawProblems()
    {
        struct FRow
        {
            const FString*         VirtualPath;
            const FCodeDiagnostic* Diagnostic;
        };
        TVector<FRow> Rows;
        for (const TUniquePtr<FDocument>& Document : Documents)
        {
            // A saved script is covered by the project build below, which also sees the files it breaks.
            if (Document->CSharp != nullptr && !Document->bDirty)
            {
                continue;
            }
            for (const FCodeDiagnostic& Diagnostic : Document->Editor.GetDiagnostics())
            {
                if (Diagnostic.Severity != ECodeSeverity::Info)
                {
                    Rows.Emplace(&Document->VirtualPath, &Diagnostic);
                }
            }
        }
        for (const FProblem& Problem : ScriptProblems)
        {
            const FDocument* Open = FindDocument(Problem.VirtualPath.View());
            if (Open == nullptr || !Open->bDirty)
            {
                Rows.Emplace(&Problem.VirtualPath, &Problem.Diagnostic);
            }
        }
        Algo::StableSort(Rows, [](const FRow& A, const FRow& B)
        {
            if (A.Diagnostic->Severity != B.Diagnostic->Severity)
            {
                return A.Diagnostic->Severity < B.Diagnostic->Severity;
            }
            const int32 Order = _stricmp(*(*A.VirtualPath), *(*B.VirtualPath));
            return Order != 0 ? Order < 0 : A.Diagnostic->Range.Start < B.Diagnostic->Range.Start;
        });

        int32 Errors = 0;
        for (const FRow& Row : Rows)
        {
            Errors += Row.Diagnostic->Severity == ECodeSeverity::Error ? 1 : 0;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Errors > 0 ? EditorColors::Danger() : EditorColors::TextDim(), LE_ICON_ALERT_CIRCLE_OUTLINE " %d errors", Errors);
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextColored(EditorColors::TextDim(), LE_ICON_ALERT_OUTLINE " %d warnings", Rows.Num() - Errors);
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::SmallButton(LE_ICON_REFRESH " Check Scripts"))
        {
            ProblemsDueAt = ImGui::GetTime();
        }
        ImGui::SetItemTooltip("Build every script in memory and list what is wrong, without reloading.");
        if (ProblemsDueAt > 0.0)
        {
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextDisabled("checking...");
        }

        constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("##problems", 3, TableFlags))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();
        int32 Clicked = -1;
        ImGuiListClipper Clipper;
        Clipper.Begin(Rows.Num());
        while (Clipper.Step())
        {
            for (int32 Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
            {
                const FRow& Row = Rows[Index];
                const bool bError = Row.Diagnostic->Severity == ECodeSeverity::Error;
                const FStringView FileName = VFS::FileName(Row.VirtualPath->View());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(Index);
                ImGui::PushStyleColor(ImGuiCol_Text, bError ? EditorColors::Danger() : EditorColors::Warning());
                ImGui::TextUnformatted(bError ? LE_ICON_ALERT_CIRCLE_OUTLINE : LE_ICON_ALERT_OUTLINE);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Selectable(*Row.Diagnostic->Message, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    Clicked = Index;
                }
                ImGui::SetItemTooltip("%s", *Row.Diagnostic->Message);
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextColored(EditorColors::TextDim(), "%.*s", FileName.Len(), FileName.GetData());
                ImGui::TableNextColumn();
                ImGui::TextColored(EditorColors::TextDim(), "%d", Row.Diagnostic->Range.Start.Line + 1);
            }
        }
        ImGui::EndTable();

        // Copied first, since opening a file can recompute the diagnostics the rows point into.
        if (Clicked >= 0)
        {
            const FString Path = *Rows[Clicked].VirtualPath;
            const FCodeRange Range = Rows[Clicked].Diagnostic->Range;
            OpenRange(Path.View(), Range);
        }
    }

    void FCodeWorkspaceTool::DrawSearch()
    {
        const ImGuiStyle& Style = ImGui::GetStyle();
        const float ToggleWidth = ImGui::CalcTextSize("Aa").x + Style.FramePadding.x * 2.0f;
        const float FieldWidth = Math::Max(80.0f, ImGui::GetContentRegionAvail().x - (ToggleWidth + Style.ItemSpacing.x) * 2.0f);

        if (bFocusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            bFocusSearch = false;
        }
        ImGui::SetNextItemWidth(FieldWidth);
        bool bRun = ImGui::InputTextWithHint("##find", LE_ICON_MAGNIFY " Find in files", SearchQuery, sizeof(SearchQuery), ImGuiInputTextFlags_EnterReturnsTrue);
        // Enter deactivates a text field, so it is focused again to let the next search be typed straight away.
        if (bRun)
        {
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        bRun |= ToggleButton("Aa", bSearchCase, "Match case.");
        ImGui::SameLine();
        bRun |= ToggleButton("W", bSearchWord, "Match whole words only.");

        ImGui::SetNextItemWidth(FieldWidth);
        ImGui::InputTextWithHint("##replace", LE_ICON_SWAP_HORIZONTAL " Replace with", SearchReplace, sizeof(SearchReplace));
        ImGui::SameLine();
        ImGui::BeginDisabled(Results.IsEmpty() || bShowingReferences);
        if (ImGui::Button("All", ImVec2(ToggleWidth * 2.0f + Style.ItemSpacing.x, 0.0f)))
        {
            bConfirmReplace = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Replace every result below.");

        ImGui::SetNextItemWidth(-FLT_MIN);
        bRun |= ImGui::InputTextWithHint("##include", "Only in files containing, such as .cs, Hud", SearchInclude, sizeof(SearchInclude), ImGuiInputTextFlags_EnterReturnsTrue);
        if (bRun)
        {
            bConfirmReplace = false;
            RunSearch();
        }

        if (bConfirmReplace)
        {
            ImGui::TextColored(EditorColors::Warning(), "Replace %d matches with '%s'?", Results.Num(), SearchReplace);
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace"))
            {
                bConfirmReplace = false;
                ReplaceAllInFiles();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
            {
                bConfirmReplace = false;
            }
        }

        ImGui::Separator();
        if (!ResultsTitle.IsEmpty())
        {
            ImGui::TextColored(EditorColors::TextDim(), "%s", *ResultsTitle);
        }
        ImGui::BeginChild("##results");
        int32 Clicked = -1;
        for (int32 Index = 0; Index < Results.Num();)
        {
            const FString& Path = Results[Index].VirtualPath;
            int32 End = Index;
            while (End < Results.Num() && Results[End].VirtualPath == Path)
            {
                ++End;
            }
            const FStringView FileName = VFS::FileName(Path.View());
            const bool bOpen = ImGui::TreeNodeEx(*Path, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s %.*s  (%d)", FileIcon(FileName), FileName.Len(), FileName.GetData(), End - Index);
            ImGui::SetItemTooltip("%s", *Path);
            if (bOpen)
            {
                for (int32 HitIndex = Index; HitIndex < End; ++HitIndex)
                {
                    const FSearchHit& Hit = Results[HitIndex];
                    if (HitRow(HitIndex, Hit.Range.Start.Line, Hit.Preview.View(), Hit.PreviewColumn, Hit.PreviewLength, Hit.bDeclaration))
                    {
                        Clicked = HitIndex;
                    }
                }
                ImGui::TreePop();
            }
            Index = End;
        }
        ImGui::EndChild();

        if (Clicked >= 0)
        {
            const FSearchHit Hit = Results[Clicked];
            const FString Target = Hit.VirtualPath.IsEmpty() ? VirtualPathOfDisk(Hit.DiskPath.View()) : Hit.VirtualPath;
            OpenRange(Target.View(), Hit.Range);
        }
    }

    void FCodeWorkspaceTool::DrawOutline()
    {
        FDocument* Active = ActiveDocument();
        if (Active == nullptr)
        {
            ImGui::TextDisabled("No file is open.");
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", LE_ICON_MAGNIFY " Filter symbols", OutlineFilter, sizeof(OutlineFilter));
        ImGui::Separator();
        ImGui::BeginChild("##symbols");
        if (Active->Outline.IsEmpty())
        {
            ImGui::TextDisabled("No symbols in this file.");
        }
        const FStringView Filter(OutlineFilter);
        const FCodeSymbolInfo* Current = EnclosingSymbol(Active->Outline, Active->Editor.GetCursor().Line);
        for (int32 Index = 0; Index < Active->Outline.Num(); ++Index)
        {
            const FCodeSymbolInfo& Symbol = Active->Outline[Index];
            if (!Filter.IsEmpty() && FuzzyScore(Filter, Symbol.Name.View()) == 0)
            {
                continue;
            }
            if (Filter.IsEmpty())
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (float)Symbol.Depth * ImGui::GetStyle().IndentSpacing);
            }
            ImGui::PushID(Index);
            if (ImGui::Selectable(*Lumina::Format("{} {}", SymbolIcon(Symbol.Kind), Symbol.Name), &Symbol == Current))
            {
                PushHistory();
                Active->Editor.SetCursor(Symbol.Position, true);
                Active->Editor.Focus();
            }
            if (!Symbol.Detail.IsEmpty())
            {
                ImGui::SetItemTooltip("%s", *Symbol.Detail);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void FCodeWorkspaceTool::DrawPicker()
    {
        constexpr const char* PopupName = "##CodePicker";
        constexpr int32 VisibleRows = 14;
        if (Picker == EPicker::None)
        {
            return;
        }
        const bool bAppearing = bPickerAppearing;
        const float Width = Math::Min(ImGui::GetWindowWidth() * 0.6f, 760.0f);
        if (bAppearing)
        {
            bPickerAppearing = false;
            ImGui::OpenPopup(PopupName);
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + (ImGui::GetWindowWidth() - Width) * 0.5f, ImGui::GetWindowPos().y + ImGui::GetFrameHeightWithSpacing() * 2.0f));
        }
        if (!ImGui::BeginPopup(PopupName))
        {
            Picker = EPicker::None;
            return;
        }

        FDocument* Active = ActiveDocument();
        const char* Hint = "Go to a file by name, or :line";
        switch (Picker)
        {
            case EPicker::WorkspaceSymbols: Hint = "Go to a type or member in the scripts"; break;
            case EPicker::DocumentSymbols:  Hint = "Go to a symbol in this file"; break;
            case EPicker::Line:             Hint = "Go to line"; break;
            default: break;
        }
        if (bAppearing)
        {
            ImGui::SetKeyboardFocusHere();
        }
        const float InnerWidth = Width - ImGui::GetStyle().WindowPadding.x * 2.0f;
        ImGui::SetNextItemWidth(InnerWidth);
        const bool bSubmit = ImGui::InputTextWithHint("##query", Hint, PickerQuery, sizeof(PickerQuery), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
        {
            PickerSelected = 0;
        }

        const FStringView Query(PickerQuery);
        TVector<FPickerRow> Rows;
        const bool bLineQuery = Picker == EPicker::Line || (Picker == EPicker::Files && !Query.IsEmpty() && Query[0] == ':');
        const int32 LineTarget = Picker == EPicker::Line ? ParseLineNumber(Query) : (bLineQuery ? ParseLineNumber(Query.Mid(1)) : -1);
        if (bLineQuery)
        {
            if (LineTarget > 0 && Active != nullptr)
            {
                const int32 Line = Math::Min(LineTarget, Active->Editor.GetLineCount());
                Rows.Add({ 1, LE_ICON_FORMAT_LIST_NUMBERED, Lumina::Format("Go to line {}", Line), Active->Name, Active->VirtualPath, { Line - 1, 0 } });
            }
        }
        else if (Picker == EPicker::Files)
        {
            for (const FFileEntry& Entry : Files)
            {
                int32 Score = Math::Max(FuzzyScore(Query, Entry.Name.View()) * 2, FuzzyScore(Query, Entry.Relative.View()));
                if (Score > 0)
                {
                    Score += Query.IsEmpty() && FindDocument(Entry.VirtualPath.View()) != nullptr ? 1000 : 0;
                    Rows.Emplace(Score, FileIcon(Entry.Name.View()), Entry.Name, Roots[Entry.Root].Label + "/" + Entry.Relative, Entry.VirtualPath);
                }
            }
        }
        else if (Picker == EPicker::DocumentSymbols && Active != nullptr)
        {
            for (const FCodeSymbolInfo& Symbol : Active->Outline)
            {
                const int32 Score = FuzzyScore(Query, Symbol.Name.View());
                if (Score > 0)
                {
                    Rows.Emplace(Score, SymbolIcon(Symbol.Kind), Symbol.Name, Symbol.Container, Active->VirtualPath, Symbol.Position);
                }
            }
        }
        else if (Picker == EPicker::WorkspaceSymbols)
        {
            FCSharpScriptLanguage* Script = AnyScriptLanguage();
            if (Script != nullptr && !Query.IsEmpty() && Query != PickerQueryUsed.View())
            {
                SyncScripts();
                TVector<FCodeSymbolInfo> Found;
                if (Script->FindWorkspaceSymbols(Query, Found) != FCSharpScriptLanguage::EReply::Busy)
                {
                    PickerSymbols = Move(Found);
                    PickerQueryUsed = Query;
                }
            }
            for (const FCodeSymbolInfo& Symbol : PickerSymbols)
            {
                const FString Target = VirtualPathOfDisk(Symbol.Path.View());
                if (!Target.IsEmpty())
                {
                    const FString Where = Lumina::Format("{}{}{}", Symbol.Container, Symbol.Container.IsEmpty() ? "" : "  ", VFS::FileName(Target.View()));
                    Rows.Emplace(Math::Max(1, FuzzyScore(Query, Symbol.Name.View())), SymbolIcon(Symbol.Kind), Symbol.Name, Where, Target, Symbol.Position);
                }
            }
            if (Script == nullptr)
            {
                ImGui::TextDisabled("This project has no C# scripts.");
            }
            else if (Query.IsEmpty())
            {
                ImGui::TextDisabled("Type part of a name.");
            }
        }
        Algo::StableSort(Rows, [](const FPickerRow& A, const FPickerRow& B) { return A.Score > B.Score; });
        if (Rows.Num() > MaxPickerRows)
        {
            Rows.SetNum(MaxPickerRows);
        }

        const int32 Count = Rows.Num();
        bool bMoved = false;
        if (Count > 0 && GetDefault<CCodeEditorShortcuts>()->MoveDown.IsPressed(0, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat))
        {
            PickerSelected = (PickerSelected + 1) % Count;
            bMoved = true;
        }
        if (Count > 0 && GetDefault<CCodeEditorShortcuts>()->MoveUp.IsPressed(0, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat))
        {
            PickerSelected = (PickerSelected + Count - 1) % Count;
            bMoved = true;
        }
        PickerSelected = Count > 0 ? Math::Clamp(PickerSelected, 0, Count - 1) : 0;

        int32 Chosen = bSubmit && Count > 0 ? PickerSelected : -1;
        const float RowHeight = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##rows", ImVec2(InnerWidth, RowHeight * (float)Math::Clamp(Count, 1, VisibleRows) + ImGui::GetStyle().WindowPadding.y));
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FPickerRow& Row = Rows[Index];
            ImGui::PushID(Index);
            if (ImGui::Selectable(*Lumina::Format("{} {}", Row.Icon, Row.Label), Index == PickerSelected))
            {
                Chosen = Index;
            }
            if (Index == PickerSelected && bMoved)
            {
                ImGui::SetScrollHereY();
            }
            if (!Row.Detail.IsEmpty())
            {
                ImGui::SameLine();
                ImGui::TextColored(EditorColors::TextDim(), "%s", *Row.Detail);
            }
            ImGui::PopID();
        }
        if (Count == 0 && !Query.IsEmpty())
        {
            ImGui::TextDisabled("Nothing matches.");
        }
        ImGui::EndChild();

        if (GetDefault<CCodeEditorShortcuts>()->Dismiss.IsPressed(0, ImGuiInputFlags_RouteAlways))
        {
            ImGui::CloseCurrentPopup();
            Picker = EPicker::None;
        }
        else if (Chosen >= 0)
        {
            const FPickerRow Target = Rows[Chosen];
            ImGui::CloseCurrentPopup();
            Picker = EPicker::None;
            ImGui::EndPopup();
            OpenDocument(Target.VirtualPath.View(), Target.Position, true);
            return;
        }
        ImGui::EndPopup();
    }

    void FCodeWorkspaceTool::DrawRenamePopup()
    {
        constexpr const char* PopupName = "##CodeRename";
        const bool bAppearing = bRenameAppearing;
        if (bAppearing)
        {
            bRenameAppearing = false;
            ImGui::OpenPopup(PopupName);
            const ImVec2 Center(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f);
            ImGui::SetNextWindowPos(Center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        if (!ImGui::BeginPopup(PopupName))
        {
            return;
        }
        int32 FileCount = 0;
        for (int32 Index = 0; Index < RenameTargets.Num(); ++Index)
        {
            FileCount += Index == 0 || RenameTargets[Index].DiskPath != RenameTargets[Index - 1].DiskPath ? 1 : 0;
        }
        ImGui::TextColored(EditorColors::SectionHeader(), "Rename '%s'", *RenameFrom);
        ImGui::TextColored(EditorColors::TextDim(), "%d places in %d files. Open files keep the change unsaved.", RenameTargets.Num(), FileCount);
        if (bAppearing)
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(360.0f);
        const bool bSubmit = ImGuiX::InputText("##name", RenameTo, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool bValid = IsIdentifier(RenameTo.View());
        if (!bValid)
        {
            ImGui::TextColored(EditorColors::Danger(), "Not a valid name.");
        }
        ImGui::BeginDisabled(!bValid);
        if (ImGui::Button(LE_ICON_CHECK " Rename") || (bSubmit && bValid))
        {
            ApplyRename();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || GetDefault<CCodeEditorShortcuts>()->Dismiss.IsPressed(0, ImGuiInputFlags_RouteAlways))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void FCodeWorkspaceTool::DrawClosePrompt()
    {
        constexpr const char* PopupName = "##CodeClose";
        if (PendingClose < 0)
        {
            return;
        }
        if (PendingClose >= Documents.Num())
        {
            PendingClose = -1;
            return;
        }
        if (!Documents[PendingClose]->bDirty)
        {
            const int32 Index = PendingClose;
            PendingClose = -1;
            CloseDocument(Index);
            return;
        }
        if (bOpenClosePrompt)
        {
            bOpenClosePrompt = false;
            ImGui::OpenPopup(PopupName);
            const ImVec2 Center(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f);
            ImGui::SetNextWindowPos(Center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        if (!ImGui::BeginPopup(PopupName))
        {
            PendingClose = -1;
            return;
        }
        FDocument& Document = *Documents[PendingClose];
        ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT_CIRCLE_OUTLINE " '%s' has unsaved changes.", *Document.Name);
        bool bClose = false;
        if (ImGui::Button(LE_ICON_CONTENT_SAVE " Save"))
        {
            bClose = SaveDocument(Document);
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            bClose = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || GetDefault<CCodeEditorShortcuts>()->Dismiss.IsPressed(0, ImGuiInputFlags_RouteAlways))
        {
            PendingClose = -1;
            ImGui::CloseCurrentPopup();
        }
        if (bClose)
        {
            const int32 Index = PendingClose;
            PendingClose = -1;
            ImGui::CloseCurrentPopup();
            CloseDocument(Index);
        }
        ImGui::EndPopup();
    }
}
