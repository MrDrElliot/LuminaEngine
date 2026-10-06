#include "Containers/StringFormat.h"
#include "GamePreviewTool.h"
#include "World/WorldManager.h"
#include "Core/Delegates/CoreDelegates.h"

namespace Lumina
{
    static FString MakeGamePreviewName(int32 ClientIndex)
    {
        return ClientIndex > 0 ? FString(Format("Client: {}", ClientIndex).c_str()) : FString("Game Preview");
    }

    FGamePreviewTool::FGamePreviewTool(IEditorToolContext* Context, CWorld* InWorld, int32 ClientIndex)
        :FEditorTool(Context, MakeGamePreviewName(ClientIndex), InWorld)
    {

    }

    void FGamePreviewTool::OnInitialize()
    {
        WorldTraveledHandle = FCoreDelegates::OnWorldTraveled.AddMember(this, &FGamePreviewTool::OnWorldTraveled);
    }

    void FGamePreviewTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        FCoreDelegates::OnWorldTraveled.Remove(WorldTraveledHandle);
    }

    // A title screen's Host or Join opens a new level, and the old world is already torn down when this runs.
    void FGamePreviewTool::OnWorldTraveled(CWorld* OldWorld, CWorld* NewWorld)
    {
        if (OldWorld == World.Get() && NewWorld != nullptr)
        {
            RebindToWorld(NewWorld);
        }
    }

    void FGamePreviewTool::Update(const FUpdateContext& UpdateContext)
    {
        
    }

    void FGamePreviewTool::SetupWorldForTool()
    {
        //FEditorTool::SetupWorldForTool();//... Don't create editor entity.
    }

    void FGamePreviewTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {

    }

    void FGamePreviewTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Play (PIE)",
            "This tool runs the duplicated PIE world. The original editor world is suspended; "
            "Stop returns to it and discards PIE-side changes.");
        DrawHelpTextRow("Input",
            "Click the viewport to focus.");
        DrawHelpTextRow("Pause",
            "Use the editor's simulation controls to pause/resume the running PIE world.");
    }

    void FGamePreviewTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), InDockspaceID);
    }

    void FGamePreviewTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        // Mirror the world editor's game-focus indicator so focus reads consistently across tools.
        DrawGameFocusIndicator(ViewportSize);
    }
}
