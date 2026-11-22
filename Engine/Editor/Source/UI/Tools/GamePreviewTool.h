#pragma once
#include "EditorTool.h"

namespace Lumina
{
    class FGamePreviewTool : public FEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FGamePreviewTool)

        // A positive ClientIndex names the tool uniquely since the tool name is the window and dock identity.
        FGamePreviewTool(IEditorToolContext* Context, CWorld* InWorld, int32 ClientIndex = 0);


        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        const char* GetTitlebarIcon() const override { return LE_ICON_EARTH; }
        void Update(const FUpdateContext& UpdateContext) override;

        void SetupWorldForTool() override;

        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        // Docked tabs cannot take game-input focus since the active-viewport switch is keyed on native window focus.
        NODISCARD bool ShouldOpenDocked() const override { return false; }

        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;

        // The game drives this camera, not an editor one.
        NODISCARD bool ShouldDrawViewGizmo() const override { return false; }

    private:

        void OnWorldTraveled(CWorld* OldWorld, CWorld* NewWorld);

        FDelegateHandle WorldTraveledHandle;

    };
}
