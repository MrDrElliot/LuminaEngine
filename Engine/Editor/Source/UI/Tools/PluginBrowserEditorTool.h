#pragma once

#include "EditorTool.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    class FPlugin;

    // Per-project plugin toggles that rewrite the .lproject Plugins array and apply next start, with no hot-unload.
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

        // A staged toggle wins, else the plugin's current state from FPluginManager.
        EDITOR_API static bool IsEffectivelyEnabled(const FPlugin* Plugin, const THashMap<FName, bool>& Pending);

        // One message per required dependency that the staged toggles leave disabled or missing under an enabled plugin.
        EDITOR_API static TVector<FString> FindBrokenDependencies(const TVector<const FPlugin*>& Plugins, const THashMap<FName, bool>& Pending);

    private:

        void DrawBrowserWindow(bool bIsFocused);
        void DrawToolbar();
        void DrawTable(const TVector<FPlugin*>& Plugins);
        void DrawDetailPanel(FPlugin* Plugin);
        void DrawFooter();

        // Stage / clear a pending toggle.
        void SetPendingEnabled(FName PluginName, bool bEnabled);

        // Returns false and fills OutError on a JSON parse or write failure.
        bool ApplyAndPersist(FString& OutError);

        // Open the "restart required" modal via the editor's modal manager.
        void PromptRestart();

        // Scaffolds into the project's Plugins folder and regenerates the project, so a project must be loaded.
        void OpenCreatePluginDialog();

    private:

        // Sparse, holding only entries that differ from the current state.
        THashMap<FName, bool> PendingChanges;

        FName    SelectedPlugin;
        FString  SearchFilter;
        bool     bShowEngine  = true;
        bool     bShowProject = true;

        // Sticky banner shown after ApplyAndPersist succeeds.
        bool     bChangesSavedBanner = false;

        // Create-plugin modal state, where a non-empty CreatePluginResult switches to the done view.
        char     NewPluginNameBuf[64]  = {};
        char     NewPluginDescBuf[160] = {};
        FString  NewPluginError;
        FString  CreatePluginResult;
    };
}
