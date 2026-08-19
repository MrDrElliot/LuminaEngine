#pragma once
#include "UI/Tools/EditorTool.h"
#include "UI/Properties/PropertyTable.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    class CClass;

    // Settings editor: discovers every CDeveloperSettings subclass via GConfig and draws each
    // through a FPropertyTable, so it matches every other reflected details panel. Edits write
    // straight back to the owning JSON file via FConfig::SaveSettings.
    class FSettingsEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FSettingsEditorTool)

        FSettingsEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Settings", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_COG; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        
    protected:
         
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        FPropertyTable* GetOrCreateTable(CClass* SettingsClass);

        CClass* SelectedClass = nullptr;
        THashMap<CClass*, TUniquePtr<FPropertyTable>> Tables;
        ImGuiTextFilter SettingsFilter;
    };
}
