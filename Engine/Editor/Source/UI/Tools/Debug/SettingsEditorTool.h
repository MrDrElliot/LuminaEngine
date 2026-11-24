#pragma once
#include "UI/Tools/EditorTool.h"
#include "UI/Properties/PropertyTable.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    class CClass;

    // Draws every CDeveloperSettings subclass through a FPropertyTable and writes edits back via FConfig::SaveSettings.
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

        void Select(CClass* SettingsClass, FStringView Search = {});
        
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
