#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Events/KeyCodes.h"

#include "MCPEditorSessionTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SUndoParams
    {
        GENERATED_BODY()

        /** How many steps to take. Stops early when the stack runs out. */
        PROPERTY()
        int32 Steps = 1;
    };

    REFLECT()
    struct MCPEDITOR_API SUndoResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Applied = 0;

        /** Label of the step editor.undo would take next, or empty. */
        PROPERTY()
        FString NextUndo;

        /** Label of the step editor.redo would take next, or empty. */
        PROPERTY()
        FString NextRedo;
    };

    REFLECT()
    struct MCPEDITOR_API SPlayStateParams
    {
        GENERATED_BODY()
    };

    REFLECT()
    struct MCPEDITOR_API SPauseParams
    {
        GENERATED_BODY()

        /** True pauses the running session, false resumes it. */
        PROPERTY()
        bool bPaused = true;
    };

    REFLECT()
    struct MCPEDITOR_API SPlayState
    {
        GENERATED_BODY()

        /** True while a play or simulate session runs. */
        PROPERTY()
        bool bPlaying = false;

        PROPERTY()
        bool bPaused = false;

        /** Path of the world the editor has open. */
        PROPERTY()
        FString World;
    };

    REFLECT()
    struct MCPEDITOR_API SScreenshotParams
    {
        GENERATED_BODY()

        /** FinalLDR writes a PNG of the tonemapped frame; SceneHDR writes a Radiance .hdr of scene color. */
        PROPERTY()
        FString Source = "FinalLDR";

        /** Where to write it. Empty picks a timestamped name under Saved/Screenshots. */
        PROPERTY()
        FString OutputPath;
    };

    REFLECT()
    struct MCPEDITOR_API SScreenshotResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        PROPERTY()
        int32 Width = 0;

        PROPERTY()
        int32 Height = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SLogTailParams
    {
        GENERATED_BODY()

        /** How many of the newest lines to return. */
        PROPERTY()
        int32 Count = 50;

        /** Only lines containing this, case-insensitive. Empty keeps every line. */
        PROPERTY()
        FString Contains;

        /** Lowest level to include: Trace, Debug, Info, Warn, Error or Critical. */
        PROPERTY()
        FString MinLevel = "Info";
    };

    REFLECT()
    struct MCPEDITOR_API SLogLine
    {
        GENERATED_BODY()

        PROPERTY()
        FString Time;

        PROPERTY()
        FString Logger;

        PROPERTY()
        FString Level;

        PROPERTY()
        FString Message;
    };

    REFLECT()
    struct MCPEDITOR_API SLogTailResult
    {
        GENERATED_BODY()

        /** Oldest first. */
        PROPERTY()
        TVector<SLogLine> Lines;

        /** How many buffered lines the filter and Count left out. */
        PROPERTY()
        int32 Dropped = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SConsoleExecParams
    {
        GENERATED_BODY()

        /** A console line: a command name, a variable name to read it, or "name value" to set it. */
        PROPERTY()
        FString Line;
    };

    REFLECT()
    struct MCPEDITOR_API SConsoleExecResult
    {
        GENERATED_BODY()

        /** The variable's value after the call, or what ran. */
        PROPERTY()
        FString Output;
    };

    REFLECT()
    struct MCPEDITOR_API STabInfo
    {
        GENERATED_BODY()

        /** Full window name, icon glyph included. */
        PROPERTY()
        FString Name;

        /** Short id to pass to editor.focus_tab and editor.close_tab. */
        PROPERTY()
        FString Id;

        /** GUID of the asset the tab edits, or empty for a panel such as the content browser. */
        PROPERTY()
        FString AssetGuid;

        PROPERTY()
        bool bUnsaved = false;

        PROPERTY()
        bool bFocused = false;

        /** False for the world editor, which never closes. */
        PROPERTY()
        bool bClosable = false;
    };

    REFLECT()
    struct MCPEDITOR_API SListTabsParams
    {
        GENERATED_BODY()
    };

    REFLECT()
    struct MCPEDITOR_API SListTabsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<STabInfo> Tabs;
    };

    REFLECT()
    struct MCPEDITOR_API SOpenAssetParams
    {
        GENERATED_BODY()

        /** GUID of the asset to open, from assets.search. A world replaces what the world editor shows. */
        PROPERTY()
        FString Asset;
    };

    REFLECT()
    struct MCPEDITOR_API SOpenAssetResult
    {
        GENERATED_BODY()

        /** Id of the tab now showing it. */
        PROPERTY()
        FString Tab;
    };

    REFLECT()
    struct MCPEDITOR_API STabNameParams
    {
        GENERATED_BODY()

        /** Tab id from editor.list_tabs, or a unique part of its name. */
        PROPERTY()
        FString Tab;
    };

    REFLECT()
    struct MCPEDITOR_API SCloseTabParams
    {
        GENERATED_BODY()

        /** Tab id from editor.list_tabs, or a unique part of its name. */
        PROPERTY()
        FString Tab;

        /** Close even when the tab has unsaved changes, which are then lost. */
        PROPERTY()
        bool bDiscardUnsaved = false;
    };

    REFLECT()
    struct MCPEDITOR_API STabActionResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bDone = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSendKeyParams
    {
        GENERATED_BODY()

        /** Key to send, by EKey name such as I, Escape, F1 or Space. */
        PROPERTY()
        EKey Key = EKey::Space;

        /** Tap presses and releases across two frames; Press or Release sends only that half. */
        PROPERTY()
        FString Action = "Tap";

        PROPERTY()
        bool bCtrl = false;

        PROPERTY()
        bool bShift = false;

        PROPERTY()
        bool bAlt = false;

        /** How long a Tap stays down, so a game polling key state sees at least one frame of it. */
        PROPERTY()
        int32 HoldMilliseconds = 100;
    };

    REFLECT()
    struct MCPEDITOR_API SSendKeyResult
    {
        GENERATED_BODY()

        /** Whether the game viewport had input focus once the key went in. */
        PROPERTY()
        bool bGameInputFocused = false;
    };

    namespace MCP
    {
        // Undo, play control, tabs and observability: what an agent needs to see and steer the editor session.
        void RegisterEditorSessionTools(FStringView Owner);
        void UnregisterEditorSessionTools();
    }
}
