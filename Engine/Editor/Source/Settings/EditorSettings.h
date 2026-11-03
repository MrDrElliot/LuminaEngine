#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Config/DeveloperSettings.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "Input/Key.h"
#include <imgui.h>
#include "EditorSettings.generated.h"

namespace Lumina
{
    // All editor settings persist into a single grouped file, one JSON section per class.

    // Transform-gizmo snapping defaults for the world editor.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "World Tool", Category = "Editor")
    class CWorldEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Whether transform gizmo snapping is enabled by default. */
        PROPERTY(Editable, Category = "Snapping")
        bool bGizmoSnapEnabled = false;

        /** Snap step (units) for the translate gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.001f, ClampMax = 100.0f)
        float GizmoSnapTranslate = 0.1f;

        /** Snap step (degrees) for the rotate gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.1f, ClampMax = 90.0f)
        float GizmoSnapRotate = 5.0f;

        /** Snap step (scale factor) for the scale gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.001f, ClampMax = 10.0f)
        float GizmoSnapScale = 0.1f;

        /** Display scale of the selected-camera preview overlay in the viewport (drag its corner to resize). */
        PROPERTY(Editable, Category = "Viewport", ClampMin = 0.25f, ClampMax = 1.5f)
        float CameraPreviewScale = 0.6f;

        /** Empty the console each time play in editor starts, so it shows only that session's output. */
        PROPERTY(Editable, Category = "Play")
        bool bClearConsoleOnPIE = false;
    };

    // Transform-gizmo snapping defaults for the prefab editor (kept distinct from the world editor).
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Prefab Tool", Category = "Editor")
    class CPrefabEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Whether transform gizmo snapping is enabled by default. */
        PROPERTY(Editable, Category = "Snapping")
        bool bGizmoSnapEnabled = false;

        /** Snap step (units) for the translate gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.001f, ClampMax = 100.0f)
        float GizmoSnapTranslate = 0.1f;

        /** Snap step (degrees) for the rotate gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.1f, ClampMax = 90.0f)
        float GizmoSnapRotate = 5.0f;

        /** Snap step (scale factor) for the scale gizmo. */
        PROPERTY(Editable, Category = "Snapping", ClampMin = 0.001f, ClampMax = 10.0f)
        float GizmoSnapScale = 0.1f;

        /** Display scale of the selected-camera preview overlay in the viewport (drag its corner to resize). */
        PROPERTY(Editable, Category = "Viewport", ClampMin = 0.25f, ClampMax = 1.5f)
        float CameraPreviewScale = 0.6f;
    };

    // Viewport ground grid, shared by every tool that draws one (world, prefab, mesh, ...).
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Viewport Grid", Category = "Editor")
    class CViewportGridSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Size of the smallest grid cell, in world units. Lines every 10 and 100 cells take over as the camera pulls back. */
        PROPERTY(Editable, Category = "Grid", ClampMin = 0.01f, ClampMax = 1000.0f)
        float Spacing = 1.0f;

        /** How far from the camera the grid fades out, in world units. It reaches further as the camera rises. */
        PROPERTY(Editable, Category = "Grid", DisplayName = "Fade Distance", ClampMin = 10.0f, ClampMax = 100000.0f)
        float Extent = 500.0f;

        /** Screen width of the grid lines, in pixels. */
        PROPERTY(Editable, Category = "Grid", ClampMin = 0.5f, ClampMax = 4.0f)
        float LineThickness = 1.0f;

        /** Color and opacity of the finest grid lines. */
        PROPERTY(Editable, Color, Category = "Grid")
        FVector4 MinorLineColor = FVector4(0.05f, 0.05f, 0.05f, 0.28f);

        /** Color and opacity of every tenth grid line. */
        PROPERTY(Editable, Color, Category = "Grid")
        FVector4 MajorLineColor = FVector4(0.05f, 0.05f, 0.05f, 0.50f);

        /** Screen width of the X and Z axis lines through the origin, in pixels. */
        PROPERTY(Editable, Category = "Axes", ClampMin = 0.5f, ClampMax = 8.0f)
        float AxisThickness = 2.0f;

        /** Draw the vertical (Y) axis line through the origin. */
        PROPERTY(Editable, Category = "Axes")
        bool bShowVerticalAxis = true;

        /** Screen thickness of the vertical axis line. */
        PROPERTY(Editable, Category = "Axes", ClampMin = 0.1f, ClampMax = 20.0f)
        float VerticalAxisThickness = 4.0f;

    };

    // External application C# script sources open in.
    REFLECT()
    enum class EScriptEditor : uint8
    {
        SystemDefault,
        VisualStudio2022,
        VisualStudio2026,
        VSCode,
        Rider,
        BuiltIn,
    };

    // Scripting preferences.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Scripting", Category = "Editor")
    class CScriptEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Application used to open script source files. System Default uses the OS file association. */
        PROPERTY(Editable, Category = "Scripts")
        EScriptEditor ScriptEditor = EScriptEditor::BuiltIn;

        /** Full path to an editor executable; overrides the choice above. Invoked as: "<exe>" "<file>". */
        PROPERTY(Editable, Category = "Scripts")
        FString CustomEditorPath;
    };

    // A command's key chord and an optional second one, such as Ctrl+Y and Ctrl+Shift+Z for redo.
    REFLECT()
    struct EDITOR_API SShortcut
    {
        GENERATED_BODY()

        SShortcut() = default;
        SShortcut(const SKey& InPrimary, const SKey& InAlternate = SKey()) : Primary(InPrimary), Alternate(InAlternate) {}

        PROPERTY(Hidden)
        SKey Primary;

        PROPERTY(Hidden)
        SKey Alternate;

        // Through ImGui's shortcut routing, so the focused editor takes a chord before the tool around it.
        bool IsPressed(ImGuiKeyChord ExtraMods = 0, ImGuiInputFlags Flags = ImGuiInputFlags_Repeat) const;

        // The primary chord for menus and tooltips, such as "Ctrl+Shift+F", or empty when unbound.
        FString GetLabel() const;
    };

    // A code editor color theme, named after its JSON file in the editor's CodeThemes folder.
    REFLECT()
    struct EDITOR_API SCodeEditorTheme
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FString Name = "Dark";
    };

    // Options every code editor shares, in the Code workspace, the RmlUi editor and the material Custom node.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Code Editor", Category = "Editor")
    class CCodeEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        PROPERTY(Editable, Category = "Theme")
        SCodeEditorTheme Theme;

        // Ctrl+wheel zooms one editor until these settings next change.
        PROPERTY(Editable, Category = "Appearance", ClampMin = 0.75f, ClampMax = 3.0f)
        float FontScale = 1.15f;

        PROPERTY(Editable, Category = "Appearance", ClampMin = 1.0f, ClampMax = 2.0f)
        float LineSpacing = 1.0f;

        PROPERTY(Editable, Category = "Appearance")
        bool bShowLineNumbers = true;

        PROPERTY(Editable, Category = "Appearance")
        bool bShowMiniMap = true;

        PROPERTY(Editable, Category = "Appearance")
        bool bShowIndentGuides = true;

        PROPERTY(Editable, Category = "Appearance")
        bool bShowWhitespace = false;

        PROPERTY(Editable, Category = "Appearance")
        bool bHighlightOccurrences = true;

        PROPERTY(Editable, Category = "Editing", ClampMin = 1, ClampMax = 8)
        int32 TabSize = 4;

        PROPERTY(Editable, Category = "Editing")
        bool bInsertSpaces = true;

        PROPERTY(Editable, Category = "Editing")
        bool bAutoIndent = true;

        PROPERTY(Editable, Category = "Editing")
        bool bMatchBrackets = true;

        PROPERTY(Editable, Category = "Editing")
        bool bAutoClosePairs = true;

        PROPERTY(Editable, Category = "Editing")
        bool bSuggestWhileTyping = true;

        PROPERTY(Editable, Category = "Editing")
        bool bTrimTrailingWhitespaceOnSave = false;
    };

    // Every key the code editors and the Code workspace answer to. Shift with a Cursor shortcut extends the selection.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Code Editor Shortcuts", Category = "Editor")
    class CCodeEditorShortcuts : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveLeft = SShortcut(SKey(EKey::Left));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveRight = SShortcut(SKey(EKey::Right));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveUp = SShortcut(SKey(EKey::Up));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveDown = SShortcut(SKey(EKey::Down));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveWordLeft = SShortcut(SKey(EKey::Left, true));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveWordRight = SShortcut(SKey(EKey::Right, true));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveToLineStart = SShortcut(SKey(EKey::Home));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveToLineEnd = SShortcut(SKey(EKey::End));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveToDocumentStart = SShortcut(SKey(EKey::Home, true));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MoveToDocumentEnd = SShortcut(SKey(EKey::End, true));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MovePageUp = SShortcut(SKey(EKey::PageUp));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut MovePageDown = SShortcut(SKey(EKey::PageDown));

        PROPERTY(Editable, Category = "Cursor")
        SShortcut JumpToMatchingBracket = SShortcut(SKey(EKey::Backslash, true, true));

        PROPERTY(Editable, Category = "Selection")
        SShortcut SelectAll = SShortcut(SKey(EKey::A, true));

        PROPERTY(Editable, Category = "Selection")
        SShortcut AddCursorAbove = SShortcut(SKey(EKey::Up, true, false, true));

        PROPERTY(Editable, Category = "Selection")
        SShortcut AddCursorBelow = SShortcut(SKey(EKey::Down, true, false, true));

        PROPERTY(Editable, Category = "Selection")
        SShortcut AddNextOccurrence = SShortcut(SKey(EKey::D, true));

        PROPERTY(Editable, Category = "Selection")
        SShortcut SelectAllOccurrences = SShortcut(SKey(EKey::L, true, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut Undo = SShortcut(SKey(EKey::Z, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut Redo = SShortcut(SKey(EKey::Y, true), SKey(EKey::Z, true, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut Cut = SShortcut(SKey(EKey::X, true), SKey(EKey::Delete, false, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut Copy = SShortcut(SKey(EKey::C, true), SKey(EKey::Insert, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut Paste = SShortcut(SKey(EKey::V, true), SKey(EKey::Insert, false, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut DeleteLeft = SShortcut(SKey(EKey::Backspace), SKey(EKey::Backspace, false, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut DeleteRight = SShortcut(SKey(EKey::Delete));

        PROPERTY(Editable, Category = "Editing")
        SShortcut DeleteWordLeft = SShortcut(SKey(EKey::Backspace, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut DeleteWordRight = SShortcut(SKey(EKey::Delete, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut NewLine = SShortcut(SKey(EKey::Enter), SKey(EKey::Enter, false, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut InsertLineBelow = SShortcut(SKey(EKey::Enter, true));

        PROPERTY(Editable, Category = "Editing")
        SShortcut InsertLineAbove = SShortcut(SKey(EKey::Enter, true, true));

        // Moves to the next snippet field while one is active, and indents a selection of whole lines.
        PROPERTY(Editable, Category = "Editing")
        SShortcut InsertTab = SShortcut(SKey(EKey::Tab));

        // Moves to the previous snippet field while one is active, and outdents otherwise.
        PROPERTY(Editable, Category = "Editing")
        SShortcut TabBack = SShortcut(SKey(EKey::Tab, false, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut MoveLinesUp = SShortcut(SKey(EKey::Up, false, false, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut MoveLinesDown = SShortcut(SKey(EKey::Down, false, false, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut DuplicateLinesUp = SShortcut(SKey(EKey::Up, false, true, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut DuplicateLinesDown = SShortcut(SKey(EKey::Down, false, true, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut DeleteLines = SShortcut(SKey(EKey::K, true, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut Indent = SShortcut(SKey(EKey::RightBracket, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut Outdent = SShortcut(SKey(EKey::LeftBracket, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut ToggleLineComment = SShortcut(SKey(EKey::Slash, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut ToggleBlockComment = SShortcut(SKey(EKey::A, false, true, true));

        PROPERTY(Editable, Category = "Lines")
        SShortcut FormatDocument = SShortcut(SKey(EKey::F, false, true, true));

        PROPERTY(Editable, Category = "View")
        SShortcut ScrollLineUp = SShortcut(SKey(EKey::Up, true));

        PROPERTY(Editable, Category = "View")
        SShortcut ScrollLineDown = SShortcut(SKey(EKey::Down, true));

        PROPERTY(Editable, Category = "View")
        SShortcut ToggleFold = SShortcut(SKey(EKey::LeftBracket, true, true));

        PROPERTY(Editable, Category = "View")
        SShortcut Unfold = SShortcut(SKey(EKey::RightBracket, true, true));

        PROPERTY(Editable, Category = "Find")
        SShortcut Find = SShortcut(SKey(EKey::F, true));

        PROPERTY(Editable, Category = "Find")
        SShortcut Replace = SShortcut(SKey(EKey::H, true));

        PROPERTY(Editable, Category = "Find")
        SShortcut FindNext = SShortcut(SKey(EKey::F3));

        PROPERTY(Editable, Category = "Find")
        SShortcut FindPrevious = SShortcut(SKey(EKey::F3, false, true));

        PROPERTY(Editable, Category = "Find")
        SShortcut GoToLine = SShortcut(SKey(EKey::G, true));

        PROPERTY(Editable, Category = "Code")
        SShortcut TriggerSuggestions = SShortcut(SKey(EKey::Space, true));

        PROPERTY(Editable, Category = "Code")
        SShortcut AcceptSuggestion = SShortcut(SKey(EKey::Enter), SKey(EKey::Tab));

        PROPERTY(Editable, Category = "Code")
        SShortcut ParameterHints = SShortcut(SKey(EKey::Space, true, true));

        PROPERTY(Editable, Category = "Code")
        SShortcut GoToDefinition = SShortcut(SKey(EKey::F12));

        PROPERTY(Editable, Category = "Code")
        SShortcut FindReferences = SShortcut(SKey(EKey::F12, false, true));

        PROPERTY(Editable, Category = "Code")
        SShortcut RenameSymbol = SShortcut(SKey(EKey::F2));

        PROPERTY(Editable, Category = "Code")
        SShortcut QuickFix = SShortcut(SKey(EKey::Period, true));

        // Closes a popup, the find bar or a snippet, or drops the extra cursors.
        PROPERTY(Editable, Category = "Code")
        SShortcut Dismiss = SShortcut(SKey(EKey::Escape));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut GoToFile = SShortcut(SKey(EKey::P, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut GoToSymbol = SShortcut(SKey(EKey::T, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut GoToSymbolInFile = SShortcut(SKey(EKey::O, true, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut FindInFiles = SShortcut(SKey(EKey::F, true, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut SaveAll = SShortcut(SKey(EKey::S, true, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut CloseFile = SShortcut(SKey(EKey::W, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut NextFile = SShortcut(SKey(EKey::PageDown, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut PreviousFile = SShortcut(SKey(EKey::PageUp, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut NavigateBack = SShortcut(SKey(EKey::Left, false, false, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut NavigateForward = SShortcut(SKey(EKey::Right, false, false, true));

        PROPERTY(Editable, Category = "Workspace")
        SShortcut ReloadScripts = SShortcut(SKey(EKey::B, true, true));
    };

    // Which tool tabs this project had open. Per-project (lives in the project's own /Config), and
    // written through on every open and close rather than at shutdown -- a crash must not lose it.
    //
    // Entries are "<kind>:<key>" strings kept in the order the tabs were opened:
    //   asset:<guid>   an asset editor, keyed by GUID so a moved or renamed asset still resolves
    //   file:<path>    a raw-file editor (.rml, ...), keyed by VFS path
    // A stringly-typed key rather than a reflected struct keeps FEditorUI from having to hand every
    // tool subclass a restore interface, and leaves the JSON readable.
    REFLECT(MinimalAPI, ConfigFile = "/Config/EditorSession.json", DisplayName = "Session", Category = "Editor")
    class CEditorSessionSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Reopen the asset and file tabs that were open the last time this project was loaded. */
        PROPERTY(Editable, Category = "Startup")
        bool bRestoreOpenTabs = true;

        /** Open tabs, oldest first. Maintained by the editor; not meant to be hand-edited. */
        PROPERTY(Hidden)
        TVector<FString> OpenTabs;

        // Files open in the Code workspace, in tab order, and the one in front.
        PROPERTY(Hidden)
        TVector<FString> CodeFiles;

        PROPERTY(Hidden)
        FString ActiveCodeFile;
    };

    // What the level editor adds to content placed into a world, decided per project.
    REFLECT(MinimalAPI, ConfigFile = "/Config/EditorProjectSettings.json", DisplayName = "Placement", Category = "Project")
    class CPlacementSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // Static meshes dropped or created into a world get a static body and a Mesh Collider; adding the component by hand never does.
        PROPERTY(Editable, Category = "Collision")
        bool bAddCollisionToPlacedMeshes = true;
    };

    // Material editor preferences.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Material Editor", Category = "Editor")
    class CMaterialEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Compile the graph automatically when the material is saved, if it has changed since the last compile. */
        PROPERTY(Editable, Category = "Compilation")
        bool bCompileOnSave = true;
    };

    // Content browser preferences.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Content Browser", Category = "Editor")
    class CContentBrowserSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Pixel size of asset tiles in the content browser. */
        PROPERTY(Editable, Category = "Layout", ClampMin = 32.0f, ClampMax = 256.0f)
        float TileSize = 86.0f;
    };

    // RmlUi editor preferences beyond the shared Code Editor ones.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "RmlUi Editor", Category = "Editor")
    class CRmlUiEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Re-parse the buffer into the preview ~250ms after each edit. */
        PROPERTY(Editable, Category = "General")
        bool bAutoReload = true;
    };
}
