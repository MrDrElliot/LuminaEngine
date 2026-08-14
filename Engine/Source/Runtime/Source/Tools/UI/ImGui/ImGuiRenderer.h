#pragma once

#include "imgui.h"
#include "ImGuiX.h"
#include "Containers/Array.h"
#include "Renderer/RHI.h"

struct ImPlotContext;

namespace Lumina
{
    class FRenderManager;
    class FUpdateContext;
}

namespace Lumina
{
    class IImGuiRenderer
    {
    public:

        virtual ~IImGuiRenderer() = default;

        virtual void Initialize();
        virtual void Deinitialize();

        void StartFrame(const FUpdateContext& UpdateContext);

        // ImGui::Render() then hand back the live draw data. Valid until the next NewFrame, which is
        // next frame's StartFrame, so the caller records from it before returning.
        ImDrawData* BuildFrame();

        virtual void OnStartFrame(const FUpdateContext& UpdateContext) = 0;

        // Draw DrawData into the acquired swapchain image (Target). The frame loop has set the heap
        // and the acquire barrier.
        virtual void OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData) {}

        // Multi-viewport: windows dragged out of the main window into their own OS windows. Each is
        // recorded from its live draw data and presented to its own swapchain.
        virtual void RenderSecondaryViewports() {}

        // Create/upload/destroy pending ImGui textures. Must run before recording, since the draw
        // lists reference the ResourceIDs it assigns.
        virtual void ProcessTextureUpdates() {}

        virtual ImTextureID GetOrCreateImTexture(FStringView Path) = 0;

        RUNTIME_API ImGuiContext* GetImGuiContext() const { return Context; }
        RUNTIME_API ImPlotContext* GetImPlotContext() const { return PlotContext; }

    protected:

        ImGuiContext* Context = nullptr;
        ImPlotContext* PlotContext = nullptr;
    };
}
