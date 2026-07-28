#pragma once

#include "Memory/SmartPtr.h"
#include "Containers/Array.h"
#include "Core/Threading/Thread.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
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
        void OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, FImDrawDataSnapshot& Snapshot) override;
        void ProcessTextureUpdates_GameThread() override;

        void CaptureSecondaryViewports_GameThread(uint8 FrameIndex) override;
        void RenderSecondaryViewports_RenderThread(uint8 FrameIndex) override;

        RUNTIME_API ImTextureID GetOrCreateImTexture(FStringView Path) override;

    private:

        // Record one ImDrawData into the swapchain image (Target) via RHI::. Clears, then one
        // DrawIndexed per ImDrawCmd with per-cmd scissor + args (vertex-pull, bindless new heap).
        void RecordDrawData_NewRHI(RHI::FCmdListH CL, ImDrawData* DrawData, RHI::FTextureH Target, const FUIntVector2& Extent);

        // Multi-viewport window-lifecycle hooks (ImGuiPlatformIO::Renderer_*), game thread, run from
        // ImGui::UpdatePlatformWindows. CreateWindow records the window and creates its window-system
        // surface (GLFW window calls are main-thread only); the swapchain itself is built lazily on the
        // render thread, which pools swapchains and so must own that allocation. DestroyWindow drains
        // the render thread and the GPU and tears everything down here, because ImGui destroys the GLFW
        // window immediately after it returns.
        static void OnRendererCreateWindow(ImGuiViewport* Viewport);
        static void OnRendererDestroyWindow(ImGuiViewport* Viewport);

        // Per-secondary-window renderer state, stored in ImGuiViewport::RendererUserData.
        struct FImGuiViewportData
        {
            void*            Window = nullptr;       // GLFWwindow*
            // Created on the game thread alongside the window (GLFW window calls are main-thread only)
            // and consumed by the render thread when it builds the swapchain.
            RHI::FSurfaceH   Surface;
            RHI::FSwapchainH Swapchain;              // created lazily on the render thread
            FUIntVector2     BuiltExtent{0, 0};      // extent the swapchain was last built for
        };

        // One ImDrawCmd, flattened with global vertex/index offsets and clip rect pre-projected to
        // framebuffer pixels (the render thread does no ImGui math).
        struct FCapturedCmd
        {
            float  ClipMinX = 0, ClipMinY = 0, ClipMaxX = 0, ClipMaxY = 0;
            uint32 TextureID = 0;
            uint32 ElemCount = 0;
            uint32 IdxOffset = 0;
            int32  VtxOffset = 0;
        };

        // A secondary viewport's whole frame, deep-copied off ImGui's live data so the render thread
        // can present it asynchronously.
        struct FCapturedViewport
        {
            FImGuiViewportData* Data = nullptr;       // not owned (lives in viewport RendererUserData)
            float               Scale[2]     = {0, 0};
            float               Translate[2] = {0, 0};
            FUIntVector2        Extent{0, 0};
            TVector<ImDrawVert> Vertices;
            TVector<uint16>     Indices;
            TVector<FCapturedCmd> Cmds;
        };

        // Render + present one captured secondary viewport (render thread).
        void RenderCapturedViewport(FCapturedViewport& Cap);

        // Pipeline (BGRA8 swapchain), depth-disabled state, and the font atlases living in the
        // new texture heap (keyed by ImTextureData::UniqueID).
        RHI::FPipelineUH                       NewPipeline;
        RHI::FDepthStencilUH                   NewDepthState;
        THashMap<int32, RHI::FManagedTexture>  NewFontTextures;

        // Path-loaded UI images (icons), decoded straight into the new texture heap so their
        // ImTextureID is a new-heap ResourceID. Cached + reused across frames.
        THashMap<FName, RHI::FManagedTexture>  PathTextures;

        // Secondary viewport draw data: captured on the game thread into the frame slot, rendered +
        // presented on the render thread, then cleared.
        TVector<FCapturedViewport>             SecondaryCaptures[RHI::kFramesInFlight];

        mutable FRecursiveMutex                Mutex;
    };
}
