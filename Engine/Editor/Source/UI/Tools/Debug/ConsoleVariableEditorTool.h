#pragma once
#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    class FConsoleVariableEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FConsoleVariableEditorTool)

        FConsoleVariableEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Console Variables", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_TUNE; }

        void OnInitialize() override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawVariablesTab();
        void DrawCommandsTab();

        bool PassesFilter(FStringView Name, FStringView Hint) const;

        char SearchBuffer[256] = {};
        FString SearchFilter;

        THashMap<FString, FString> StringEditBuffers;
    };
}
