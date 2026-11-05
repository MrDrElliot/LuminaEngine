#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Events/KeyCodes.h"

#include "MCPEditorSessionTools.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EEditorHistoryAction : uint8
    {
        Undo,
        Redo,
    };

    REFLECT()
    struct MCPEDITOR_API SEditorHistoryParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEditorHistoryAction Action = EEditorHistoryAction::Undo;

        // How many steps to take. Stops early when the stack runs out.
        PROPERTY()
        int32 Steps = 1;

        // Any open tab, such as a material or animation graph editor. Empty uses the scene target.
        PROPERTY()
        FString Tab;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorHistoryResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Applied = 0;

        // Label of the step Undo would take next, or empty.
        PROPERTY()
        FString NextUndo;

        // Label of the step Redo would take next, or empty.
        PROPERTY()
        FString NextRedo;
    };

    REFLECT()
    enum class EEditorPlayAction : uint8
    {
        Start,
        Stop,
        Pause,
        Resume,
        State,
    };

    REFLECT()
    struct MCPEDITOR_API SEditorPlayParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEditorPlayAction Action = EEditorPlayAction::Start;

        // Start only. 0 keeps the current setting. Players past the first join player one as clients.
        PROPERTY()
        int32 Players = 0;

        // Start only. standalone, listen or dedicated. Empty keeps the current setting.
        PROPERTY()
        FString NetMode;
    };

    REFLECT()
    struct MCPEDITOR_API SPlayState
    {
        GENERATED_BODY()

        // True while a play or simulate session runs.
        PROPERTY()
        bool bPlaying = false;

        PROPERTY()
        bool bPaused = false;

        // Path of the world the editor has open.
        PROPERTY()
        FString World;

        // Detached to the editor's free camera with F8 while the game keeps running.
        PROPERTY()
        bool bEjected = false;

        // False after Shift+F1 hands input back to the editor.
        PROPERTY()
        bool bGameInputFocused = false;

        // Normal, Hidden or Captured.
        PROPERTY()
        FString MouseMode;

        // The active viewport's input mode: Game, UI or GameAndUI.
        PROPERTY()
        FString InputMode;
    };

    REFLECT()
    struct MCPEDITOR_API SSaveAllParams
    {
        GENERATED_BODY()
    };

    REFLECT()
    struct MCPEDITOR_API SSaveAllResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Saved = 0;

        // Package paths that refused to save and are still dirty.
        PROPERTY()
        TVector<FString> Failed;

        // True when the open level is a transient world with no package, which no save reaches.
        PROPERTY()
        bool bWorldUnsaved = false;
    };

    REFLECT()
    struct MCPEDITOR_API SQuitParams
    {
        GENERATED_BODY()

        // Save every dirty package first, and stay open if any of them fails.
        PROPERTY()
        bool bSaveFirst = false;

        // Quit even with unsaved changes, which are then lost.
        PROPERTY()
        bool bDiscardUnsaved = false;
    };

    REFLECT()
    struct MCPEDITOR_API SQuitResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bQuitting = false;

        // What kept the editor open, when it refused.
        PROPERTY()
        TVector<FString> Unsaved;
    };

    REFLECT()
    enum class EEditorTabsAction : uint8
    {
        List,
        Focus,
        Close,
        Float,
        Target,
    };

    REFLECT()
    struct MCPEDITOR_API SEditorTabsParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEditorTabsAction Action = EEditorTabsAction::List;

        // Tab id from List, or a unique part of its name. Target with an empty Tab restores the world editor.
        PROPERTY()
        FString Tab;

        // Close only. Close even with unsaved changes, which are then lost.
        PROPERTY()
        bool bDiscardUnsaved = false;

        // Float only. Screen position of the floating window's top-left corner.
        PROPERTY()
        int32 X = 100;

        PROPERTY()
        int32 Y = 100;

        // Float only.
        PROPERTY()
        int32 Width = 800;

        PROPERTY()
        int32 Height = 600;
    };

    REFLECT()
    struct MCPEDITOR_API STabInfo
    {
        GENERATED_BODY()

        // Full window name, icon glyph included.
        PROPERTY()
        FString Name;

        // Short id to pass as Tab.
        PROPERTY()
        FString Id;

        // GUID of the asset the tab edits, or empty for a panel such as the content browser.
        PROPERTY()
        FString AssetGuid;

        PROPERTY()
        bool bUnsaved = false;

        PROPERTY()
        bool bFocused = false;

        // False for the world editor, which never closes.
        PROPERTY()
        bool bClosable = false;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorTabsResult
    {
        GENERATED_BODY()

        // List only.
        PROPERTY()
        TVector<STabInfo> Tabs;

        // True once Focus, Close, Float or Target was applied or queued.
        PROPERTY()
        bool bDone = false;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorOpenParams
    {
        GENERATED_BODY()

        // Asset GUID or path to open in its editor tab. A world replaces what the world editor shows.
        PROPERTY()
        FString Asset;

        // Virtual path of a text file with an editor of its own, such as an .rml or .rcss.
        PROPERTY()
        FString File;

        // World path to travel the running game to, such as /Game/Content/Maps/Venezuela, with ?listen or ?port=N.
        PROPERTY()
        FString Level;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorOpenResult
    {
        GENERATED_BODY()

        // Id of the tab now showing Asset or File.
        PROPERTY()
        FString Tab;

        // The map Level is traveling to.
        PROPERTY()
        FString Map;
    };

    REFLECT()
    enum class EEditorInputKind : uint8
    {
        Text,
        Mouse,
        Key,
    };

    REFLECT()
    struct MCPEDITOR_API SEditorInputParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEditorInputKind Kind = EEditorInputKind::Text;

        // Text only. Sent as character input, the way a keyboard layout delivers typed text to a focused field.
        PROPERTY()
        FString Text;

        // Key only. EKey name such as I, Escape, F1 or Space.
        PROPERTY()
        EKey Key = EKey::Space;

        // Tap presses and releases across two frames. Press or Release sends one half. Move is Mouse only.
        PROPERTY()
        FString Action = "Tap";

        // Mouse only. A pixel of the game view as editor.screenshot saves it, wherever the viewport is docked.
        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        // Mouse only. 0 is left, 1 is right, 2 is middle.
        PROPERTY()
        int32 Button = 0;

        // Key only.
        PROPERTY()
        bool bCtrl = false;

        PROPERTY()
        bool bShift = false;

        PROPERTY()
        bool bAlt = false;

        // How long a Tap stays down so a polling game sees it. Zero uses 40 for Mouse and 100 for Key.
        PROPERTY()
        int32 HoldMilliseconds = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorInputResult
    {
        GENERATED_BODY()

        // Whether the game viewport had input focus once the input went in.
        PROPERTY()
        bool bGameInputFocused = false;

        // Mouse only. Where the cursor was left, in viewport pixels.
        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;
    };

    REFLECT()
    enum class EEditorGraphAction : uint8
    {
        View,
        Frame,
        Arrange,
    };

    REFLECT()
    struct MCPEDITOR_API SEditorGraphParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEditorGraphAction Action = EEditorGraphAction::View;

        // Graph editor tab id from editor.tabs, or a unique part of its name.
        PROPERTY()
        FString Tab;

        // Frame only. Node ids to frame and select. Empty frames every node.
        PROPERTY()
        TVector<int64> Nodes;
    };

    REFLECT()
    struct MCPEDITOR_API SEditorGraphResult
    {
        GENERATED_BODY()

        // Frame and Arrange: queued for the tab's next frame.
        PROPERTY()
        bool bDone = false;

        // View only, as are the fields below. False until the canvas has been drawn once.
        PROPERTY()
        bool bDrawn = false;

        PROPERTY()
        float MinX = 0.0f;

        PROPERTY()
        float MinY = 0.0f;

        PROPERTY()
        float MaxX = 0.0f;

        PROPERTY()
        float MaxY = 0.0f;

        PROPERTY()
        float Zoom = 1.0f;

        PROPERTY()
        int32 NodeCount = 0;

        PROPERTY()
        TVector<int64> Offscreen;
    };

    REFLECT()
    struct MCPEDITOR_API SBrowseFolderParams
    {
        GENERATED_BODY()

        // Content folder to show, such as /Game/Content/Items.
        PROPERTY()
        FString Folder;

        // Typed into the search box. Empty clears it.
        PROPERTY()
        FString Search;
    };

    REFLECT()
    struct MCPEDITOR_API SBrowseFolderResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bDone = false;
    };

    REFLECT()
    struct MCPEDITOR_API SScriptReloadParams
    {
        GENERATED_BODY()

        // Seconds to wait for the reload to land. Zero queues it and returns.
        PROPERTY()
        float TimeoutSeconds = 30.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SScriptReloadResult
    {
        GENERATED_BODY()

        // False when the wait ran out with the reload still queued.
        PROPERTY()
        bool bReloaded = false;

        PROPERTY()
        int32 PreviousGeneration = 0;

        // Advances once per completed reload.
        PROPERTY()
        int32 Generation = 0;

        // Collectible script contexts still loaded. More than one means an old one has not unloaded.
        PROPERTY()
        int32 AliveContexts = 0;

        // Entity script types the new generation registered, zero when compilation failed.
        PROPERTY()
        int32 LoadedTypes = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SScreenshotParams
    {
        GENERATED_BODY()

        // FinalLDR (tonemapped PNG), SceneHDR (Radiance .hdr) or EditorUI (PNG of the whole editor window).
        PROPERTY()
        FString Source = "FinalLDR";

        // Empty picks a timestamped name under Saved/Screenshots.
        PROPERTY()
        FString OutputPath;

        // Capture this tab's own world, such as an asset editor's preview, instead of the active one.
        PROPERTY()
        FString Tab;
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

        // How many of the newest lines to return.
        PROPERTY()
        int32 Count = 50;

        // Only lines containing this, case-insensitive.
        PROPERTY()
        FString Contains;

        // Trace, Debug, Info, Warn, Error or Critical.
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

        // Oldest first.
        PROPERTY()
        TVector<SLogLine> Lines;

        // How many buffered lines the filter and Count left out.
        PROPERTY()
        int32 Dropped = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SConsoleExecParams
    {
        GENERATED_BODY()

        // A command name, a variable name to read it, or "name value" to set it.
        PROPERTY()
        FString Line;
    };

    REFLECT()
    struct MCPEDITOR_API SConsoleExecResult
    {
        GENERATED_BODY()

        // The variable's value after the call, or "executed" for a command.
        PROPERTY()
        FString Output;
    };

    REFLECT()
    struct MCPEDITOR_API SWindowParams
    {
        GENERATED_BODY()

        // Query, Resize, Move, Minimize, Restore or Maximize.
        PROPERTY()
        FString Action = "Query";

        // Client size in pixels, for Resize.
        PROPERTY()
        int32 Width = 0;

        PROPERTY()
        int32 Height = 0;

        // Screen position of the top-left corner, for Move.
        PROPERTY()
        int32 X = 0;

        PROPERTY()
        int32 Y = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SWindowState
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Width = 0;

        PROPERTY()
        int32 Height = 0;

        PROPERTY()
        int32 X = 0;

        PROPERTY()
        int32 Y = 0;

        PROPERTY()
        bool bMinimized = false;

        PROPERTY()
        bool bMaximized = false;
    };

    namespace MCP
    {
        void RegisterEditorSessionTools(FStringView Owner);
        void UnregisterEditorSessionTools();
    }
}
