#pragma once

#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/DevUI/DevConsole.h"

namespace Lumina
{
    // A game build's developer overlay, opened with the grave key, holding the console and every world's OnDebugGui windows.
    class FGameDevUI : public IDevelopmentToolUI
    {
    public:

        void Initialize(const FUpdateContext& UpdateContext) override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;
        void OnEndFrame(const FUpdateContext& UpdateContext) override;
        bool OnEvent(FEvent& Event) override;

        RUNTIME_API bool IsOpen() const { return bOpen; }
        RUNTIME_API void SetOpen(bool bInOpen);

        // Saves the next presented frame, overlay included, under the project's Saved/Screenshots.
        RUNTIME_API void RequestScreenshot();

    private:

        // A closed overlay still draws, so ImGui is told to leave the mouse, keyboard and cursor to the game.
        void ApplyImGuiInputFlags();

        void RunPendingExecLines();
        void FinishPendingScreenshot();

        FDevConsole      Console;
        // Lines from -exec, run one a frame once a game world has had time to come up.
        TVector<FString> PendingExecLines;
        int32            FramesWithGameWorld = 0;
        uint64           PendingScreenshotTicket = 0;
        bool             bScreenshotPending = false;
        // Off by default, so closing the overlay hides every script's debug window with it.
        bool             bKeepScriptWindowsVisible = false;
        bool             bOpen = false;
        bool             bFocusConsoleNextFrame = false;
    };
}
