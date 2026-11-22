#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include "ImGuiAllocator.h"

// ImGui is a static lib, so each plugin binary has its own globals; invoke this once at file scope so the module manager points them at the engine's shared copy.
#define LUMINA_MODULE_IMGUI()                                                                    \
    extern "C" DLL_EXPORT void LuminaModuleSetupImGui(void* InImGui, void* InImPlot)             \
    {                                                                                            \
        ::Lumina::ImGuiX::AdoptImGuiContext(static_cast<ImGuiContext*>(InImGui), static_cast<ImPlotContext*>(InImPlot)); \
    }                                                                                            \
    static_assert(true, "LUMINA_MODULE_IMGUI consumes the semicolon at the call site")

namespace Lumina::ImGuiX
{
    // Inline, so it sets the globals of whichever binary calls it.
    inline void AdoptImGuiContext(ImGuiContext* InImGui, ImPlotContext* InImPlot)
    {
        ImGui::SetCurrentContext(InImGui);
        ImPlot::SetCurrentContext(InImPlot);
        InstallImGuiAllocator();
        // Only the ImGui copy that builds the font atlas fills its word-wrap tables, so this one breaks text mid-word until told to.
        ImTextInitClassifiers();
    }

    // The Runtime DLL owns the context it creates, so only a host that made its own (a headless test) needs this.
    RUNTIME_API void AdoptRuntimeImGuiContext(ImGuiContext* InImGui, ImPlotContext* InImPlot);
}
