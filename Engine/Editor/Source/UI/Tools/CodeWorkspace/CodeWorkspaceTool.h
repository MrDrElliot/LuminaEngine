#pragma once

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/Languages/CSharpScriptLanguage.h"
#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    // The in-engine IDE, one workspace that holds every script and text file the project opens.
    class FCodeWorkspaceTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FCodeWorkspaceTool)

        explicit FCodeWorkspaceTool(IEditorToolContext* Context);

        // Whether a file of this kind opens here when the project asks for an editor, rather than in a dedicated tool.
        static bool Handles(FStringView VirtualPath);

        // Opens or focuses a file, then puts the caret on Reveal when it is given.
        void OpenDocument(FStringView VirtualPath, FCodePosition Reveal = { -1, 0 }, bool bRecordHistory = true);

        const char* GetTitlebarIcon() const override { return LE_ICON_CODE_BRACES; }
        bool IsUnsavedDocument() override;
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnSave() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        struct FDocument
        {
            FString                   VirtualPath;
            FString                   DiskPath;
            FString                   Name;
            TUniquePtr<FCodeLanguage> Language;
            // Set when Language is the compiler-backed C#, which the workspace asks for references and project problems.
            FCSharpScriptLanguage*    CSharp = nullptr;
            FCodeEditor               Editor;
            FString                   SavedText;
            uint64                    CheckedVersion = ~0ull;
            uint64                    OutlineVersion = ~0ull;
            double                    OutlineCheckedAt = -1.0;
            TVector<FCodeSymbolInfo>  Outline;
            bool                      bDirty = false;
        };

        struct FFileEntry
        {
            FString VirtualPath;
            FString DiskPath;
            FString Name;
            // Path below its root, which the tree splits into folders and quick open matches against.
            FString Relative;
            int32   Root = 0;
        };

        struct FRoot
        {
            FString Label;
            FString VirtualPath;
            FString DiskPath;
        };

        struct FTreeNode
        {
            FString        Name;
            int32          File = -1;
            TVector<int32> Children;
        };

        struct FLocation
        {
            FString       VirtualPath;
            FCodePosition Position;
        };

        struct FSearchHit
        {
            FString    VirtualPath;
            FString    DiskPath;
            FCodeRange Range;
            FString    Preview;
            bool       bDeclaration = false;
            // Where the match sits inside Preview, which is the line with its indent trimmed.
            int32      PreviewColumn = -1;
            int32      PreviewLength = 0;
        };

        struct FProblem
        {
            FString         VirtualPath;
            FCodeDiagnostic Diagnostic;
        };

        enum class EPicker : uint8
        {
            None,
            Files,
            WorkspaceSymbols,
            DocumentSymbols,
            Line,
        };

        FDocument* FindDocument(FStringView VirtualPath);
        FDocument* ActiveDocument();
        FDocument* CreateDocument(FStringView VirtualPath);
        void LoadDocument(FDocument& Document);
        bool SaveDocument(FDocument& Document);
        void SaveAll();
        void CloseDocument(int32 Index);
        void RequestClose(int32 Index);
        void UpdateDocuments();
        void RememberSession() const;
        void RestoreSession();

        void RebuildIndex();
        void StartWatching();
        FString VirtualPathOfDisk(FStringView DiskPath) const;

        void Navigate(const FLocation& Location, bool bRecordHistory);
        void PushHistory();
        void GoBack();
        void GoForward();
        void OpenDiskLocation(FStringView DiskPath, FCodeRange Range);
        void OpenRange(FStringView VirtualPath, FCodeRange Range);

        void SyncScripts();
        FCSharpScriptLanguage* AnyScriptLanguage();
        void FindReferences(FDocument& Document, FCodePosition Position);
        void BeginRename(FDocument& Document, FCodePosition Position);
        void ApplyRename();
        void RefreshProblems();
        void ApplyEditsToFile(FStringView DiskPath, TVector<FCodeTextEdit> Edits);

        void RunSearch();
        void ReplaceAllInFiles();

        void HandleShortcuts();
        void DrawExplorer();
        void DrawTree(int32 Node, const FString& Folder);
        void DrawNewFilePopup();
        void DrawEditorArea();
        void DrawToolbar();
        void DrawStatusBar(FDocument& Document);
        void DrawProblems();
        void DrawSearch();
        void DrawOutline();
        void DrawPicker();
        void DrawRenamePopup();
        void DrawClosePrompt();
        void OpenPicker(EPicker Kind);

        TVector<TUniquePtr<FDocument>> Documents;
        int32                          ActiveIndex = -1;
        // A tab to select on the next frame, since a tab bar only switches tabs while it is being drawn.
        int32                          SelectRequest = -1;
        int32                          PendingClose = -1;
        bool                           bOpenClosePrompt = false;

        TVector<FRoot>                 Roots;
        TVector<FFileEntry>            Files;
        TVector<FTreeNode>             TreeNodes;
        TVector<int32>                 RootNodes;
        TAtomic<bool>                  bIndexDirty{ false };
        // Disk paths the watchers saw change, drained on the game thread to reload open documents.
        FMutex                         ChangedMutex;
        TVector<FString>               ChangedPaths;
        TVector<TUniquePtr<FDirectoryWatcher>> Watchers;
        char                           ExplorerFilter[128] = {};
        FString                        NewFileFolder;
        char                           NewFileBuffer[128] = {};
        bool                           bNewFileAppearing = false;

        TVector<FLocation>             BackHistory;
        TVector<FLocation>             ForwardHistory;

        // The search panel lists either a find in files or the references to a symbol.
        char                           SearchQuery[256] = {};
        char                           SearchReplace[256] = {};
        char                           SearchInclude[128] = {};
        bool                           bSearchCase = false;
        bool                           bSearchWord = false;
        bool                           bFocusSearch = false;
        bool                           bShowingReferences = false;
        bool                           bRevealSearch = false;
        bool                           bRevealProblems = false;
        bool                           bConfirmReplace = false;
        FString                        ResultsTitle;
        TVector<FSearchHit>            Results;

        TVector<FProblem>              ScriptProblems;
        double                         ProblemsDueAt = 0.0;
        int32                          ReloadsSeen = 0;
        int32                          ReloadsAtSave = -1;

        EPicker                        Picker = EPicker::None;
        bool                           bPickerAppearing = false;
        char                           PickerQuery[256] = {};
        FString                        PickerQueryUsed;
        int32                          PickerSelected = 0;
        TVector<FCodeSymbolInfo>       PickerSymbols;
        FCSharpScriptLanguage          WorkspaceLanguage;

        bool                           bRenameAppearing = false;
        char                           RenameBuffer[128] = {};
        FString                        RenameFrom;
        TVector<FSearchHit>            RenameTargets;

        char                           OutlineFilter[128] = {};
    };
}
