#pragma once

#include "imgui.h"
#include "ImGuiX.h"
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

        // An overlay draws over the game image and keeps to the game window, where the editor's owns the whole swapchain.
        explicit IImGuiRenderer(bool bInOverlay = false) : bOverlay(bInOverlay) {}
        virtual ~IImGuiRenderer()   = default;
        LE_NO_COPYMOVE(IImGuiRenderer);

        virtual void Initialize();
        virtual void Deinitialize();

        void StartFrame(const FUpdateContext& UpdateContext);

        // Returns the live draw data, which stays valid only until the next StartFrame.
        ImDrawData* BuildFrame();

        virtual void OnStartFrame(const FUpdateContext& UpdateContext) = 0;

        // Draws into the acquired swapchain image, with the heap and acquire barrier already set by the frame loop.
        virtual void OnEndFrame(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData) {}

        // Multi-viewport windows dragged out of the main window, each presented to its own swapchain.
        virtual void RenderSecondaryViewports() {}

        // Must run before recording since the draw lists reference the ResourceIDs it assigns.
        virtual void ProcessTextureUpdates() {}

        virtual ImTextureID GetOrCreateImTexture(FStringView Path) = 0;

        RUNTIME_API ImGuiContext* GetImGuiContext() const { return Context; }

        // Between NewFrame and Render, the only window in which a widget call is legal.
        RUNTIME_API bool IsFrameOpen() const { return bFrameOpen; }
        bool IsOverlay() const { return bOverlay; }
        RUNTIME_API ImPlotContext* GetImPlotContext() const { return PlotContext; }

    protected:

        ImGuiContext* Context = nullptr;
        ImPlotContext* PlotContext = nullptr;

        // ImGui keeps only a pointer to the layout path, so the renderer owns the string.
        FString LayoutFilePath;

        bool bOverlay = false;
        bool bFrameOpen = false;
    };
}
