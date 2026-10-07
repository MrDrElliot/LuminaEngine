#pragma once

namespace Lumina::ImGuiX::NativeFrames
{
    // Floating windows keep their ImGui look but get a native frame, so the OS can maximize, snap and resize them.
    void Install();

    // Draws the window controls and records each frame's caption area, once per frame before ImGui::Render.
    void Update();

    // Runs the minimize, maximize and restore clicks queued this frame, after ImGui::UpdatePlatformWindows.
    void FlushWindowActions();
}
