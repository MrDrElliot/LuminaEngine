#pragma once

#include <imgui.h>
#include <implot.h>
#include "ImGuiAllocator.h"

// ImGui is a StaticLib, so every binary that links it gets its OWN copy of ImGui's global state:
// the current context (GImGui), the ImPlot context, and the allocator function pointers. A plugin
// DLL therefore starts with a null context + default allocator, and its first ImGui call crashes
// (null GImGui deref, or a cross-heap free of memory the engine allocated).
//
// An editor module/plugin that draws ImGui opts in by invoking this macro ONCE at file scope in its
// module .cpp. It defines an exported hook that the module manager calls, automatically, once the
// engine's ImGui context is ready (FModuleManager::NotifyImGuiReady), and for modules loaded after
// that, at load time. The hook runs IN the module's binary, so it points that binary's ImGui/ImPlot
// globals + allocator at the engine's single shared copy (ImGuiMemAlloc/Free route through the
// RUNTIME_API Memory::Malloc, so all ImGui memory lands in the one engine heap).
//
// Mirrors the manual setup the directly-linked Editor module does in FEditorUI::Initialize.
#define LUMINA_MODULE_IMGUI()                                                                    \
    extern "C" __declspec(dllexport) void LuminaModuleSetupImGui(void* InImGui, void* InImPlot)  \
    {                                                                                            \
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(InImGui));                           \
        ImPlot::SetCurrentContext(static_cast<ImPlotContext*>(InImPlot));                        \
        ::Lumina::ImGuiX::InstallImGuiAllocator();                                               \
    }
