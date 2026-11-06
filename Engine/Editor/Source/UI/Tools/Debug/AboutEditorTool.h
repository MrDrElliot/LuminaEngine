#pragma once
#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    class FAboutEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FAboutEditorTool)

        FAboutEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "About Lumina", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CIRCLE; }

        void OnInitialize() override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawAboutTab();
        void DrawContributorsTab();
    };
}
