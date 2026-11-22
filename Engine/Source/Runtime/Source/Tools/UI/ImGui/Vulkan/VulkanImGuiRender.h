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

    // Vulkan ImGui backend.
    class FVulkanImGuiRender : public IImGuiRenderer
    {
    public:

        explicit FVulkanImGuiRender(bool bInOverlay = false) : IImGuiRenderer(bInOverlay) {}

        void Initialize() override;
        void Deinitialize() override;

        void OnStartFrame(const FUpdateContext& UpdateContext) override;
        void OnEndFrame(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData) override;
        void ProcessTextureUpdates() override;

        void RenderSecondaryViewports() override;

        RUNTIME_API ImTextureID GetOrCreateImTexture(FStringView Path) override;

    private:

        // Clears the swapchain image, then issues one DrawIndexed per ImDrawCmd.
        void RecordDrawData_NewRHI(RHI::FCmdListH CL, ImDrawData* DrawData, RHI::FTextureH Target, const FUIntVector2& Extent);

        // Runs inside an already-open render pass and is shared by the main viewport and secondary windows.
        void RecordDrawLists(RHI::FCmdListH CL, ImDrawData* DrawData, EFormat TargetFormat, float FBW, float FBH);

        // Scene images draw opaque, so the HDR gain in their alpha lands in the target unblended.
        RHI::FPipelineH GetPipeline(EFormat TargetFormat, bool bOpaque);

        // Multi-viewport window-lifecycle hooks.
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

        // Keyed by target format and opaque bit, since the window may draw into an SDR swapchain or the HDR composite.
        THashMap<uint64, RHI::FPipelineH>      Pipelines;
        // The font atlases living in the new texture heap, keyed by ImTextureData::UniqueID.
        THashMap<int32, RHI::FManagedTexture>  NewFontTextures;

        // Path-loaded UI icons decoded into the new texture heap and cached across frames.
        THashMap<FName, RHI::FManagedTexture>  PathTextures;

        mutable FRecursiveMutex                Mutex;

        // Set while a game cursor image shows, so ImGui leaves the OS cursor alone without undoing a flag someone else set.
        bool                                   bHoldingCursorChange = false;
    };
}
