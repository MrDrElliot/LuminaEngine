#pragma once

#include "EditorTool.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    class FPlugin;

    // View + per-project enable/disable for plugins. "Apply & Restart" rewrites the .lproject
    // "Plugins" array; takes effect next start (no hot-unload). Read-only with no project loaded.
    class FPluginBrowserEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FPluginBrowserEditorTool)

        FPluginBrowserEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Plugin Browser", nullptr)
        {}

        bool        IsSingleWindowTool()   const override { return true; }
        const char* GetTitlebarIcon()      const override;

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        void DrawBrowserWindow(bool bIsFocused);
        void DrawToolbar();
        void DrawTable(const TVector<FPlugin*>& Plugins);
        void DrawDetailPanel(FPlugin* Plugin);
        void DrawFooter();

        // Desired-enabled state: a staged toggle wins, else the plugin's
        // current state from FPluginManager.
        bool IsEffectivelyEnabled(const FPlugin* Plugin) const;

        // Stage / clear a pending toggle.
        void SetPendingEnabled(FName PluginName, bool bEnabled);

        // Persist staged toggles to <Project>/<Name>.lproject. Returns false
        // + populates OutError on JSON parse / write failure.
        bool ApplyAndPersist(FString& OutError);

        // Open the "restart required" modal via the editor's modal manager.
        void PromptRestart();

        // Open the "create plugin" modal (name + description → scaffold into
        // <Project>/Plugins/ and regenerate the project). Project must be loaded.
        void OpenCreatePluginDialog();

    private:

        // Plugin name (FName) → desired enabled. Sparse: only contains
        // entries that differ from the current state.
        THashMap<FName, bool> PendingChanges;

        FName    SelectedPlugin;
        FString  SearchFilter;
        bool     bShowEngine  = true;
        bool     bShowProject = true;

        // After ApplyAndPersist succeeds, sticky banner that says
        // "<N> change(s) saved; restart to apply".
        bool     bChangesSavedBanner = false;

        // Create-plugin modal state. CreatePluginResult holds the post-create
        // confirmation; non-empty switches the modal to its "done" view.
        char     NewPluginNameBuf[64]  = {};
        char     NewPluginDescBuf[160] = {};
        FString  NewPluginError;
        FString  CreatePluginResult;
    };
}
