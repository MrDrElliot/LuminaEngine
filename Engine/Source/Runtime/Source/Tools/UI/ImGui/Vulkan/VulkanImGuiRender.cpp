#include "RuntimePCH.h"
#include "VulkanImGuiRender.h"

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"

#include <GLFW/glfw3.h>
#include <algorithm>

#include "Core/Engine/Engine.h"
#include "Core/Profiler/Profile.h"
#include "Core/Windows/Window.h"
#include "Memory/Memcpy.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina
{
    // The single live backend instance, so the static ImGuiPlatformIO::Renderer_* hooks can reach it.
    static FVulkanImGuiRender* GImGuiBackend = nullptr;

    // Mirrors FImGuiArgs in ImGuiVert/Pixel.slang (32 B scalar).
    struct FNewImGuiArgs
    {
        float  Scale[2];
        float  Translate[2];
        uint32 TextureID;
        uint32 SamplerIndex;
        uint64 VertexAddr;
    };

    void FVulkanImGuiRender::Initialize()
    {
        IImGuiRenderer::Initialize();
        LUMINA_PROFILE_SCOPE();

        ImGui_ImplGlfw_InitForVulkan(Windowing::GetPrimaryWindowHandle()->GetWindow(), true);

        ImGuiIO& IO = ImGui::GetIO();
        IO.BackendRendererName = "Lumina_RHI";
        IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // 16-bit indices + VtxOffset for >64K meshes
        IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // ImTextureData create/update/destroy path
        IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;  // we drive secondary-window swapchains

        // Multi-viewport: ImGui strips ViewportsEnable on the first NewFrame unless both Platform and
        // Renderer advertise viewport support. We render secondary viewports ourselves in
        // RenderSecondaryViewports, so only the window-lifecycle hooks are registered;
        // RenderPlatformWindowsDefault is intentionally not used.
        ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
        PlatformIO.Renderer_CreateWindow  = &FVulkanImGuiRender::OnRendererCreateWindow;
        PlatformIO.Renderer_DestroyWindow = &FVulkanImGuiRender::OnRendererDestroyWindow;
        GImGuiBackend = this;

        // The VS pulls ImDrawVert from the transient ring by device address; the shader hard-codes
        // the field offsets, so pin the layout here.
        static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout drifted; ImGuiVert.slang must be updated.");
    }

    void FVulkanImGuiRender::Deinitialize()
    {
        RHI::WaitDeviceIdle();

        FRecursiveScopeLock Lock(Mutex);

        NewPipeline.Reset();
        NewDepthState.Reset();
        for (auto& KV : NewFontTextures)
        {
            RHI::Textures::Release(KV.second);
        }
        NewFontTextures.clear();

        for (auto& KV : PathTextures)
        {
            RHI::Textures::Release(KV.second);
        }
        PathTextures.clear();

        // ImGui_ImplGlfw_Shutdown / DestroyContext tear down any remaining secondary windows, which
        // calls OnRendererDestroyWindow back into us; keep GImGuiBackend valid until after.
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        GImGuiBackend = nullptr;
    }

    void FVulkanImGuiRender::OnStartFrame(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        {
            LUMINA_PROFILE_SECTION_COLORED("ImGui_ImplGlfw_NewFrame", tracy::Color::Aquamarine);
            ImGui_ImplGlfw_NewFrame();
        }
        {
            LUMINA_PROFILE_SECTION_COLORED("ImGui::NewFrame", tracy::Color::Aquamarine);
            ImGui::NewFrame();
        }
    }

    void FVulkanImGuiRender::ProcessTextureUpdates()
    {
        LUMINA_PROFILE_SCOPE();
        FRecursiveScopeLock Lock(Mutex);

        // Create + upload the font atlas into the new texture heap, so its ImTextureID is a
        // new-heap ResourceID the new-RHI ImGui shaders sample directly.
        for (ImTextureData* Tex : ImGui::GetPlatformIO().Textures)
        {
            if (Tex->Status == ImTextureStatus_WantCreate || Tex->Status == ImTextureStatus_WantUpdates)
            {
                auto It = NewFontTextures.find(Tex->UniqueID);
                if (It == NewFontTextures.end())
                {
                    RHI::FManagedTexture Created = RHI::Textures::Create(RHI::FTexture2DDesc
                    {
                        .Width  = (uint32)Tex->Width,
                        .Height = (uint32)Tex->Height,
                        .Format = EFormat::RGBA8_UNORM,
                    });
                    It = NewFontTextures.insert_or_assign(Tex->UniqueID, Created).first;
                    Tex->SetTexID((ImTextureID)(uint32)Created.ResourceID());
                }

                if (Tex->GetPixels() != nullptr)
                {
                    const uint64 Bytes = (uint64)Tex->Width * (uint64)Tex->Height * 4;
                    RHI::Textures::Upload(It->second, 0, Tex->GetPixels(), Bytes, (uint32)Tex->Width);
                }

                Tex->SetStatus(ImTextureStatus_OK);
            }
            else if (Tex->Status == ImTextureStatus_WantDestroy && Tex->UnusedFrames >= (int)(RHI::kFramesInFlight * 2))
            {
                auto It = NewFontTextures.find(Tex->UniqueID);
                if (It != NewFontTextures.end())
                {
                    RHI::Textures::Release(It->second);
                    NewFontTextures.erase(It);
                }
                Tex->SetTexID(ImTextureID_Invalid);
                Tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void FVulkanImGuiRender::OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData)
    {
        RecordDrawData_NewRHI(CL, DrawData, Target, Extent);
    }

    void FVulkanImGuiRender::RecordDrawData_NewRHI(RHI::FCmdListH CL, ImDrawData* DrawData, RHI::FTextureH Target, const FUIntVector2& Extent)
    {
        if (!NewPipeline)
        {
            RHI::FBlendDesc Blend;
            Blend.bBlendEnable   = true;
            Blend.ColorOp        = RHI::EBlend::Add;
            Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.AlphaOp        = RHI::EBlend::Add;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

            const RHI::FColorTarget ColorTarget { .Format = EFormat::BGRA8_UNORM, .Blend = Blend };
            RHI::FRasterDesc RasterDesc;
            RasterDesc.Topology     = RHI::ETopology::TriangleList;
            RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

            NewPipeline = RHI::Core::CreateGraphicsPipeline("ImGuiVert.slang", "ImGuiPixel.slang", RasterDesc);
        }
        if (!NewDepthState)
        {
            NewDepthState = RHI::CreateDepthStencil(RHI::FDepthStencilDesc{});
        }

        // Always begin/end the pass so the swapchain image is cleared even with no draws.
        RHI::FRenderAttachment Color;
        Color.Texture  = Target;
        Color.LoadOp   = RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;
        Color.Color[0] = Color.Color[1] = Color.Color[2] = 0.0f;
        Color.Color[3] = 1.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);

        RecordDrawLists(CL, DrawData, (float)Extent.x, (float)Extent.y);

        RHI::CmdEndRenderPass(CL);
    }

    void FVulkanImGuiRender::RecordDrawLists(RHI::FCmdListH CL, ImDrawData* DrawData, float FBW, float FBH)
    {
        if (DrawData != nullptr && DrawData->TotalVtxCount > 0 && DrawData->TotalIdxCount > 0 && FBW > 0.0f && FBH > 0.0f && NewPipeline)
        {
            const int32 TotalVtx = DrawData->TotalVtxCount;
            const int32 TotalIdx = DrawData->TotalIdxCount;

            RHI::FTransientAlloc VB = RHI::Core::AllocTransient((size_t)TotalVtx * sizeof(ImDrawVert), 16);
            RHI::FTransientAlloc IB = RHI::Core::AllocTransient((size_t)TotalIdx * sizeof(ImDrawIdx), 4);

            ImDrawVert* VtxDst = static_cast<ImDrawVert*>(VB.Cpu);
            ImDrawIdx*  IdxDst = static_cast<ImDrawIdx*>(IB.Cpu);
            for (int32 n = 0; n < DrawData->CmdListsCount; ++n)
            {
                const ImDrawList* List = DrawData->CmdLists[n];
                Memory::Memcpy(VtxDst, List->VtxBuffer.Data, (size_t)List->VtxBuffer.Size * sizeof(ImDrawVert));
                Memory::Memcpy(IdxDst, List->IdxBuffer.Data, (size_t)List->IdxBuffer.Size * sizeof(ImDrawIdx));
                VtxDst += List->VtxBuffer.Size;
                IdxDst += List->IdxBuffer.Size;
            }

            FNewImGuiArgs Args;
            Args.Scale[0]     = 2.0f / DrawData->DisplaySize.x;
            Args.Scale[1]     = 2.0f / DrawData->DisplaySize.y;
            Args.Translate[0] = -1.0f - DrawData->DisplayPos.x * Args.Scale[0];
            Args.Translate[1] = -1.0f - DrawData->DisplayPos.y * Args.Scale[1];
            Args.SamplerIndex = (uint32)RHI::EStockSampler::LinearWrap;
            Args.VertexAddr   = VB.Gpu;

            RHI::CmdSetDepthStencilState(CL, NewDepthState.Get());
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CCW);
            RHI::CmdSetPipeline(CL, NewPipeline.Get());

            const ImVec2 ClipOff   = DrawData->DisplayPos;
            const ImVec2 ClipScale = DrawData->FramebufferScale;
            const uint32 DefaultTex = RHI::Textures::DefaultResourceID();

            uint32 GlobalVtx = 0, GlobalIdx = 0;
            for (int32 n = 0; n < DrawData->CmdListsCount; ++n)
            {
                const ImDrawList* List = DrawData->CmdLists[n];
                for (int32 c = 0; c < List->CmdBuffer.Size; ++c)
                {
                    const ImDrawCmd& Cmd = List->CmdBuffer[c];
                    if (Cmd.UserCallback != nullptr)
                    {
                        continue;
                    }

                    float MinX = std::max((Cmd.ClipRect.x - ClipOff.x) * ClipScale.x, 0.0f);
                    float MinY = std::max((Cmd.ClipRect.y - ClipOff.y) * ClipScale.y, 0.0f);
                    float MaxX = std::min((Cmd.ClipRect.z - ClipOff.x) * ClipScale.x, FBW);
                    float MaxY = std::min((Cmd.ClipRect.w - ClipOff.y) * ClipScale.y, FBH);
                    if (MaxX <= MinX || MaxY <= MinY)
                    {
                        continue;
                    }

                    RHI::CmdSetScissor(CL, RHI::FRect{ (int)MinX, (int)MaxX, (int)MinY, (int)MaxY });

                    const int32 RawTexID = (int32)Cmd.GetTexID();
                    Args.TextureID = (RawTexID >= 0) ? (uint32)RawTexID : DefaultTex;
                    const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(Args);

                    RHI::CmdDrawIndexed(CL, IB.Gpu, 0, ArgsPtr, Cmd.ElemCount, 1,
                                        Cmd.IdxOffset + GlobalIdx, (int32)(Cmd.VtxOffset + GlobalVtx), 0, RHI::EIndexType::Uint16);
                }
                GlobalVtx += List->VtxBuffer.Size;
                GlobalIdx += List->IdxBuffer.Size;
            }
        }
    }

    void FVulkanImGuiRender::OnRendererCreateWindow(ImGuiViewport* Viewport)
    {
        if (GImGuiBackend == nullptr || Viewport->PlatformHandle == nullptr)
        {
            return;
        }

        // The swapchain is built lazily on first render (RenderSecondaryViewport); the window-system
        // surface is created here because GLFW's window calls are main-thread only.
        FImGuiViewportData* Data = IM_NEW(FImGuiViewportData)();
        Data->Window  = Viewport->PlatformHandle;
        Data->Surface = RHI::CreateSurface(Data->Window);
        Viewport->RendererUserData = Data;
    }

    void FVulkanImGuiRender::OnRendererDestroyWindow(ImGuiViewport* Viewport)
    {
        FImGuiViewportData* Data = static_cast<FImGuiViewportData*>(Viewport->RendererUserData);
        if (Data == nullptr)
        {
            return;
        }

        // ImGui destroys the GLFW window (and its surface) right after this hook. Recording is
        // synchronous, so only submitted GPU work can still reference the swapchain.
        if (RHI::IsValid(Data->Swapchain))
        {
            RHI::WaitDeviceIdle();
            RHI::FreeH(Data->Swapchain);
        }
        else
        {
            // Never rendered, so nothing consumed it: the surface is still ours to destroy. (CreateSwapchain
            // consumes the handle, so this must not run once a swapchain exists.)
            RHI::FreeH(Data->Surface);
        }

        IM_DELETE(Data);
        Viewport->RendererUserData = nullptr;
    }

    void FVulkanImGuiRender::RenderSecondaryViewports()
    {
        LUMINA_PROFILE_SCOPE();

        ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
        for (int32 i = 1; i < PlatformIO.Viewports.Size; ++i)   // index 0 is the main viewport
        {
            ImGuiViewport* Viewport = PlatformIO.Viewports[i];
            RenderSecondaryViewport(static_cast<FImGuiViewportData*>(Viewport->RendererUserData), Viewport->DrawData);
        }
    }

    void FVulkanImGuiRender::RenderSecondaryViewport(FImGuiViewportData* Data, ImDrawData* DrawData)
    {
        if (Data == nullptr || Data->Window == nullptr || DrawData == nullptr || DrawData->TotalVtxCount == 0)
        {
            return;
        }

        const float ReqW = DrawData->DisplaySize.x * DrawData->FramebufferScale.x;
        const float ReqH = DrawData->DisplaySize.y * DrawData->FramebufferScale.y;
        if (ReqW <= 0.0f || ReqH <= 0.0f)
        {
            return;
        }

        const FUIntVector2 RequestedExtent((uint32)ReqW, (uint32)ReqH);

        // Create / resize the secondary swapchain, tracking the built extent so a clamped surface
        // doesn't thrash recreation every frame.
        if (!RHI::IsValid(Data->Swapchain))
        {
            if (!RHI::IsValid(Data->Surface))
            {
                return;
            }
            Data->Swapchain   = RHI::CreateSwapchain(Data->Surface, RequestedExtent);
            Data->Surface     = RHI::FSurfaceH{};
            Data->BuiltExtent = RequestedExtent;
        }
        else if (Data->BuiltExtent != RequestedExtent)
        {
            RHI::RecreateSwapchain(Data->Swapchain, RequestedExtent);
            Data->BuiltExtent = RequestedExtent;
        }

        // Surface collapsed to zero area (minimized / mid-drag) so the swapchain did not build; retry next frame.
        const FUIntVector2 BuiltExtent = RHI::GetSwapchainExtent(Data->Swapchain);
        if (BuiltExtent.x == 0 || BuiltExtent.y == 0)
        {
            Data->BuiltExtent = FUIntVector2(0, 0);
            return;
        }

        RHI::FTextureH Img = RHI::AcquireNextImage(Data->Swapchain);
        if (!RHI::IsValid(Img))
        {
            RHI::RecreateSwapchain(Data->Swapchain, RequestedExtent);
            Data->BuiltExtent = RequestedExtent;
            return;   // retry next frame
        }

        const FUIntVector2 ImgExtent = RHI::GetSwapchainExtent(Data->Swapchain);

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        RHI::CmdSwapchainBarrierToRender(CL, Data->Swapchain);

        RHI::FRenderAttachment Color;
        Color.Texture  = Img;
        Color.LoadOp   = RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;
        Color.Color[0] = Color.Color[1] = Color.Color[2] = 0.0f;
        Color.Color[3] = 1.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = ImgExtent;
        RHI::CmdBeginRenderPass(CL, Pass);

        // Clip rects project against the actual image, which can be smaller than the requested size
        // when the surface clamped.
        RecordDrawLists(CL, DrawData, (float)ImgExtent.x, (float)ImgExtent.y);

        RHI::CmdEndRenderPass(CL);
        RHI::Core::Present(Data->Swapchain, CL);
    }

    ImTextureID FVulkanImGuiRender::GetOrCreateImTexture(FStringView Path)
    {
        FRecursiveScopeLock Lock(Mutex);

        const FName NamePath = Path;
        if (auto It = PathTextures.find(NamePath); It != PathTextures.end())
        {
            return (ImTextureID)(uint32)It->second.ResourceID();
        }

        // Decode straight into the new texture heap (no old-RHI image), so the ImTextureID is a
        // new-heap ResourceID the new-RHI ImGui shaders sample directly.
        TOptional<Import::Textures::FTextureImportResult> Result = Import::Textures::ImportTexture(Path, false);
        if (!Result.has_value() || Result->Pixels.empty())
        {
            return (ImTextureID)(uint32)RHI::Textures::DefaultResourceID();
        }

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result->Dimensions.x,
            .Height = Result->Dimensions.y,
            .Format = Result->Format,
        });
        RHI::Textures::Upload(Texture, 0, Result->Pixels.data(), Result->Pixels.size(), Result->Dimensions.x);

        const uint32 ResourceID = Texture.ResourceID();
        PathTextures.insert_or_assign(NamePath, Move(Texture));
        return (ImTextureID)(uint32)ResourceID;
    }

}
