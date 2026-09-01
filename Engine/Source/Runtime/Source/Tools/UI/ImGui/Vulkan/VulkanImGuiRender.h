#pragma once

#include "Memory/SmartPtr.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Thread.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Renderer/SwapchainTarget.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"

struct ImDrawData;
struct ImTextureData;
struct ImDrawVert;
struct ImGuiViewport;

namespace Lumina
{
    class FUpdateContext;

    // New-RHI ImGui backend: records draws via RHI:: into the swapchain image, samples the
    // global texture heap by ResourceID. ImGui_ImplGlfw kept for input; one DrawIndexed per
    // ImDrawCmd, vertex-pull by device address. Fonts live in RHI::Textures (new heap).
    class FVulkanImGuiRender : public IImGuiRenderer
    {
    public:

        void Initialize() override;
        void Deinitialize() override;

        void OnStartFrame(const FUpdateContext& UpdateContext) override;
        void OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData) override;
        void ProcessTextureUpdates() override;

        void RenderSecondaryViewports() override;

        RUNTIME_API ImTextureID GetOrCreateImTexture(FStringView Path) override;

    private:

        // Record one ImDrawData into the swapchain image (Target) via RHI::. Clears, then one
        // DrawIndexed per ImDrawCmd with per-cmd scissor + args (vertex-pull, bindless new heap).
        void RecordDrawData_NewRHI(RHI::FCmdListH CL, ImDrawData* DrawData, RHI::FTextureH Target, const FUIntVector2& Extent);

        // Draw-list upload + per-cmd DrawIndexed, inside an already-open render pass. Shared by the
        // main viewport and every secondary window.
        void RecordDrawLists(RHI::FCmdListH CL, ImDrawData* DrawData, float FBW, float FBH);

        // Multi-viewport window-lifecycle hooks (ImGuiPlatformIO::Renderer_*), run from
        // ImGui::UpdatePlatformWindows. CreateWindow records the window and creates its window-system
        // surface (GLFW window calls are main-thread only); the swapchain is built lazily on first
        // render. DestroyWindow waits for the GPU and tears everything down here, because ImGui
        // destroys the GLFW window immediately after it returns.
        static void OnRendererCreateWindow(ImGuiViewport* Viewport);
        static void OnRendererDestroyWindow(ImGuiViewport* Viewport);

        // Per-secondary-window renderer state, stored in ImGuiViewport::RendererUserData.
        struct FImGuiViewportData
        {
            void*                 Window = nullptr;   // GLFWwindow*
            RHI::FSwapchainTarget Target;
        };

        // Build/resize the window's swapchain, record its live draw data, present.
        void RenderSecondaryViewport(FImGuiViewportData* Data, ImDrawData* DrawData);

        // Pipeline (BGRA8 swapchain), depth-disabled state, and the font atlases living in the
        // new texture heap (keyed by ImTextureData::UniqueID).
        RHI::FPipelineUH                       NewPipeline;
        RHI::FDepthStencilH                    NewDepthState;
        THashMap<int32, RHI::FManagedTexture>  NewFontTextures;

        // Path-loaded UI images (icons), decoded straight into the new texture heap so their
        // ImTextureID is a new-heap ResourceID. Cached + reused across frames.
        THashMap<FName, RHI::FManagedTexture>  PathTextures;

        mutable FRecursiveMutex                Mutex;
    };
}
