#include "pch.h"
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
#include "Renderer/RenderThread.h"
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
        // Renderer advertise viewport support. We render secondary viewports ourselves (capture on the
        // game thread, present on the render thread), so only the window-lifecycle hooks are registered;
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

        for (TVector<FCapturedViewport>& Slot : SecondaryCaptures)
        {
            Slot.clear();
        }

        // ImGui_ImplGlfw_Shutdown / DestroyContext tear down any remaining secondary windows, which
        // calls OnRendererDestroyWindow back into us; keep GImGuiBackend valid until after.
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ClearSnapshots();
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

    void FVulkanImGuiRender::ProcessTextureUpdates_GameThread()
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

    void FVulkanImGuiRender::OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, FImDrawDataSnapshot& Snapshot)
    {
        RecordDrawData_NewRHI(CL, Snapshot.GetDrawData(), Target, Extent);
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

        const float FBW = (float)Extent.x;
        const float FBH = (float)Extent.y;

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

        RHI::CmdEndRenderPass(CL);
    }

    void FVulkanImGuiRender::OnRendererCreateWindow(ImGuiViewport* Viewport)
    {
        if (GImGuiBackend == nullptr || Viewport->PlatformHandle == nullptr)
        {
            return;
        }

        // The swapchain is created lazily on the render thread (first RenderCapturedViewport), but the
        // window-system surface is created here: GLFW's window calls are main-thread only, and the
        // render thread must not touch a GLFWwindow the platform backend may be creating or destroying.
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

        // ImGui destroys the GLFW window (and its surface) right after this hook, so nothing on the
        // render thread may still reference this viewport. Captures hold a raw FImGuiViewportData* and
        // a torn-out window can be closed in the very frame it was first captured -- before its
        // swapchain was ever built -- so the drain is unconditional, not gated on a valid swapchain.
        // (Gating it left the render thread to read freed FImGuiViewportData and hand a dangling
        // GLFWwindow* to glfwCreateWindowSurface inside RHI::CreateSwapchain.)
        FlushRenderingCommands();

        // Captured but not yet consumed: the capture happens before the frame's render command is
        // enqueued, so a flush alone does not guarantee the slot was drained. Scrub explicitly.
        if (GImGuiBackend != nullptr)
        {
            FRecursiveScopeLock Lock(GImGuiBackend->Mutex);
            for (TVector<FCapturedViewport>& Slot : GImGuiBackend->SecondaryCaptures)
            {
                Slot.erase(std::remove_if(Slot.begin(), Slot.end(),
                    [Data](const FCapturedViewport& Cap) { return Cap.Data == Data; }), Slot.end());
            }
        }

        if (RHI::IsValid(Data->Swapchain))
        {
            RHI::WaitDeviceIdle();
            RHI::FreeH(Data->Swapchain);
        }
        else
        {
            // Never reached the render thread: the surface is still ours to destroy. (CreateSwapchain
            // consumes the handle, so this must not run once a swapchain exists.)
            RHI::FreeH(Data->Surface);
        }

        IM_DELETE(Data);
        Viewport->RendererUserData = nullptr;
    }

    void FVulkanImGuiRender::CaptureSecondaryViewports_GameThread(uint8 FrameIndex)
    {
        LUMINA_PROFILE_SCOPE();

        const uint8 Slot = FrameIndex % RHI::kFramesInFlight;
        TVector<FCapturedViewport>& Captures = SecondaryCaptures[Slot];
        Captures.clear();

        ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
        for (int32 i = 1; i < PlatformIO.Viewports.Size; ++i)   // index 0 is the main viewport
        {
            ImGuiViewport*      Viewport = PlatformIO.Viewports[i];
            FImGuiViewportData* Data     = static_cast<FImGuiViewportData*>(Viewport->RendererUserData);
            ImDrawData*         DrawData = Viewport->DrawData;
            if (Data == nullptr || DrawData == nullptr || DrawData->TotalVtxCount == 0)
            {
                continue;
            }

            const float FbW = DrawData->DisplaySize.x * DrawData->FramebufferScale.x;
            const float FbH = DrawData->DisplaySize.y * DrawData->FramebufferScale.y;
            if (FbW <= 0.0f || FbH <= 0.0f)
            {
                continue;
            }

            Captures.emplace_back();
            FCapturedViewport& Cap = Captures.back();
            Cap.Data         = Data;
            Cap.Scale[0]     = 2.0f / DrawData->DisplaySize.x;
            Cap.Scale[1]     = 2.0f / DrawData->DisplaySize.y;
            Cap.Translate[0] = -1.0f - DrawData->DisplayPos.x * Cap.Scale[0];
            Cap.Translate[1] = -1.0f - DrawData->DisplayPos.y * Cap.Scale[1];
            Cap.Extent       = FUIntVector2((uint32)FbW, (uint32)FbH);
            Cap.Vertices.reserve(DrawData->TotalVtxCount);
            Cap.Indices.reserve(DrawData->TotalIdxCount);

            const ImVec2 ClipOff   = DrawData->DisplayPos;
            const ImVec2 ClipScale = DrawData->FramebufferScale;
            const uint32 DefaultTex = RHI::Textures::DefaultResourceID();
            uint32 GlobalVtx = 0, GlobalIdx = 0;
            for (int32 n = 0; n < DrawData->CmdListsCount; ++n)
            {
                const ImDrawList* List = DrawData->CmdLists[n];
                Cap.Vertices.insert(Cap.Vertices.end(), List->VtxBuffer.Data, List->VtxBuffer.Data + List->VtxBuffer.Size);
                Cap.Indices.insert(Cap.Indices.end(),   List->IdxBuffer.Data, List->IdxBuffer.Data + List->IdxBuffer.Size);

                for (int32 c = 0; c < List->CmdBuffer.Size; ++c)
                {
                    const ImDrawCmd& Cmd = List->CmdBuffer[c];
                    if (Cmd.UserCallback != nullptr)
                    {
                        continue;   // user callbacks aren't replayed for secondary viewports
                    }

                    const float MinX = std::max((Cmd.ClipRect.x - ClipOff.x) * ClipScale.x, 0.0f);
                    const float MinY = std::max((Cmd.ClipRect.y - ClipOff.y) * ClipScale.y, 0.0f);
                    const float MaxX = std::min((Cmd.ClipRect.z - ClipOff.x) * ClipScale.x, FbW);
                    const float MaxY = std::min((Cmd.ClipRect.w - ClipOff.y) * ClipScale.y, FbH);
                    if (MaxX <= MinX || MaxY <= MinY)
                    {
                        continue;
                    }

                    FCapturedCmd CC;
                    CC.ClipMinX  = MinX; CC.ClipMinY = MinY; CC.ClipMaxX = MaxX; CC.ClipMaxY = MaxY;
                    const int32 TexID = (int32)Cmd.GetTexID();
                    CC.TextureID = (TexID >= 0) ? (uint32)TexID : DefaultTex;
                    CC.ElemCount = Cmd.ElemCount;
                    CC.IdxOffset = Cmd.IdxOffset + GlobalIdx;
                    CC.VtxOffset = (int32)(Cmd.VtxOffset + GlobalVtx);
                    Cap.Cmds.push_back(CC);
                }
                GlobalVtx += List->VtxBuffer.Size;
                GlobalIdx += List->IdxBuffer.Size;
            }
        }
    }

    void FVulkanImGuiRender::RenderSecondaryViewports_RenderThread(uint8 FrameIndex)
    {
        LUMINA_PROFILE_SCOPE();

        const uint8 Slot = FrameIndex % RHI::kFramesInFlight;
        for (FCapturedViewport& Cap : SecondaryCaptures[Slot])
        {
            RenderCapturedViewport(Cap);
        }
        SecondaryCaptures[Slot].clear();
    }

    void FVulkanImGuiRender::RenderCapturedViewport(FCapturedViewport& Cap)
    {
        FImGuiViewportData* Data = Cap.Data;
        if (Data == nullptr || Data->Window == nullptr || Cap.Indices.empty() || Cap.Extent.x == 0 || Cap.Extent.y == 0)
        {
            return;
        }

        // Create / resize the secondary swapchain on the render thread (the only thread that touches
        // the swapchain pool during a frame), tracking the built extent so a clamped surface doesn't
        // thrash recreation every frame. The surface itself came from the game thread; consuming it
        // here means no window-system call happens off the window's own thread.
        if (!RHI::IsValid(Data->Swapchain))
        {
            if (!RHI::IsValid(Data->Surface))
            {
                return;
            }
            Data->Swapchain   = RHI::CreateSwapchain(Data->Surface, Cap.Extent);
            Data->Surface     = RHI::FSurfaceH{};
            Data->BuiltExtent = Cap.Extent;
        }
        else if (Data->BuiltExtent != Cap.Extent)
        {
            RHI::RecreateSwapchain(Data->Swapchain, Cap.Extent);
            Data->BuiltExtent = Cap.Extent;
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
            RHI::RecreateSwapchain(Data->Swapchain, Cap.Extent);
            Data->BuiltExtent = Cap.Extent;
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

        const float FBW = (float)ImgExtent.x;
        const float FBH = (float)ImgExtent.y;

        if (NewPipeline && FBW > 0.0f && FBH > 0.0f)
        {
            const size_t VBytes = Cap.Vertices.size() * sizeof(ImDrawVert);
            const size_t IBytes = Cap.Indices.size()  * sizeof(uint16);
            RHI::FTransientAlloc VB = RHI::Core::AllocTransient(VBytes, 16);
            RHI::FTransientAlloc IB = RHI::Core::AllocTransient(IBytes, 4);
            Memory::Memcpy(VB.Cpu, Cap.Vertices.data(), VBytes);
            Memory::Memcpy(IB.Cpu, Cap.Indices.data(),  IBytes);

            FNewImGuiArgs Args;
            Args.Scale[0]     = Cap.Scale[0];
            Args.Scale[1]     = Cap.Scale[1];
            Args.Translate[0] = Cap.Translate[0];
            Args.Translate[1] = Cap.Translate[1];
            Args.SamplerIndex = (uint32)RHI::EStockSampler::LinearWrap;
            Args.VertexAddr   = VB.Gpu;

            RHI::CmdSetDepthStencilState(CL, NewDepthState.Get());
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CCW);
            RHI::CmdSetPipeline(CL, NewPipeline.Get());

            for (const FCapturedCmd& CC : Cap.Cmds)
            {
                // Clip rects were captured against the requested size, which can exceed the clamped
                // swapchain extent; re-clamp to the actual image.
                const float MinX = std::min(CC.ClipMinX, FBW);
                const float MinY = std::min(CC.ClipMinY, FBH);
                const float MaxX = std::min(CC.ClipMaxX, FBW);
                const float MaxY = std::min(CC.ClipMaxY, FBH);
                if (MaxX <= MinX || MaxY <= MinY)
                {
                    continue;
                }

                RHI::CmdSetScissor(CL, RHI::FRect{ (int)MinX, (int)MaxX, (int)MinY, (int)MaxY });
                Args.TextureID = CC.TextureID;
                const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(Args);

                RHI::CmdDrawIndexed(CL, IB.Gpu, 0, ArgsPtr, CC.ElemCount, 1,
                                    CC.IdxOffset, CC.VtxOffset, 0, RHI::EIndexType::Uint16);
            }
        }

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
