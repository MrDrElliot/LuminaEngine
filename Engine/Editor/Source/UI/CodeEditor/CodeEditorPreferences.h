#pragma once

namespace Lumina
{
    class IEditorToolContext;

    namespace CodeEditorPreferences
    {
        // The everyday Code Editor settings as controls for a tool's menu, saved as they change, with links to the full pages.
        EDITOR_API void DrawQuickSettings(IEditorToolContext* Tools);

        // Every code editor shortcut with the keys bound to it now, grouped by category.
        EDITOR_API void DrawShortcutReference();
    }
}
