#pragma once

#include "Core/Threading/Atomic.h"
#include "Platform/Filesystem/DirectoryWatcher.h"
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/Languages/CSharpScriptLanguage.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    // Edits a C# script in the editor, with suggestions and errors from the compiler that builds the scripts.
    class FScriptEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FScriptEditorTool)

        FScriptEditorTool(IEditorToolContext* Context, FStringView InVirtualPath);

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_LANGUAGE_CSHARP; }
        bool WantsAutomaticAssetUndo() override { return false; }
        bool IsUnsavedDocument() override { return bDirty; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnSave() override;

        // Puts the caret on Position once the script has loaded, for a definition found from another script.
        void Reveal(FCodePosition Position) { PendingReveal = Position; }

    private:

        void LoadFromDisk();
        void StartWatching();
        void DrawToolbar();
        void DrawStatusBar();
        void DrawGotoLinePopup();
        void OpenDefinition(const FCodeLocation& Location);

        FString               VirtualPath;
        FString               DiskPath;
        FCSharpScriptLanguage Language;
        FCodeEditor           CodeEditor;

        FString               SavedText;
        uint64                CheckedVersion = ~0ull;
        bool                  bDirty = false;

        // A definition to show once the text is in, since another script's editor asks before this one has loaded.
        FCodePosition         PendingReveal{ -1, 0 };

        // The script reload a save asked for, so its outcome is reported once it finishes.
        int32                 ReloadsBeforeSave = -1;

        int32                 GotoLineBuffer = 1;
        bool                  bRequestGoto = false;

        FDelegateHandle       FileRenamedHandle;
        FDirectoryWatcher     FileWatcher;
        TAtomic<bool>         bExternalChangePending{ false };
    };
}
