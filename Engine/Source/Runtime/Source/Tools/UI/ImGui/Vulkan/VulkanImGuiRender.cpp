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

    // Mirrors FImGuiArgs in ImGuiVert/Pixel.slang (48 B scalar).
    struct FNewImGuiArgs
    {
        float  Scale[2];
        float  Translate[2];
        uint32 TextureID;
        uint32 SamplerIndex;
        uint32 DisplayMode;
        float  Exposure;
        uint32 ArraySlice;
        uint32 bIsArray;
        uint64 VertexAddr;
    };
    static_assert(sizeof(FNewImGuiArgs) == 48, "FNewImGuiArgs must match ImGuiCommon.slang::FImGuiArgs.");

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

        // Secondary viewports render here, so the default platform-window render is intentionally unused.
        ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
        PlatformIO.Renderer_CreateWindow  = &FVulkanImGuiRender::OnRendererCreateWindow;
        PlatformIO.Renderer_DestroyWindow = &FVulkanImGuiRender::OnRendererDestroyWindow;
        GImGuiBackend = this;

        // The shader hard-codes the field offsets, so the vertex layout is pinned here.
        static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout drifted; ImGuiVert.slang must be updated.");
    }

    void FVulkanImGuiRender::Deinitialize()
    {
        RHI::WaitDeviceIdle();

        FRecursiveScopeLock Lock(Mutex);

        NewPipeline.Reset();
        RHI::FreeH(NewDepthState);
        NewDepthState = {};
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

        // Teardown calls back into the destroy-window hook, so the backend stays valid until after.
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

        // So the atlas texture id is a new-heap resource id the ImGui shaders sample directly.
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
                        .DebugName = "ImGui.FontAtlas",
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
            Args.DisplayMode  = 0;      // cooked textures are already encoded
            Args.Exposure     = 1.0f;
            Args.ArraySlice   = 0;
            Args.bIsArray     = 0;      // the overwhelming majority of ImGui draws are plain Texture2D
            Args.VertexAddr   = VB.Gpu;

            // Resolved once, since the preview stamps this marker and the draws match on identity.
            const ImDrawCallback DisplayStateCallback = ImGuiX::Detail::GetDisplayStateCallback();

            RHI::CmdSetDepthStencilState(CL, NewDepthState);
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
                        // It carries no geometry, so it updates state for the draws that follow and contributes nothing.
                        if (Cmd.UserCallback == DisplayStateCallback &&
                            Cmd.UserCallbackData != nullptr &&
                            Cmd.UserCallbackDataSize == (int)sizeof(ImGuiX::Detail::FImGuiDisplayState))
                        {
                            const auto* State = static_cast<const ImGuiX::Detail::FImGuiDisplayState*>(Cmd.UserCallbackData);
                            Args.DisplayMode = State->DisplayMode;
                            Args.Exposure    = State->Exposure;
                            Args.ArraySlice  = State->ArraySlice;
                            Args.bIsArray    = State->bIsArray;
                        }
                        continue;
                    }

                    float MinX = Math::Max((Cmd.ClipRect.x - ClipOff.x) * ClipScale.x, 0.0f);
                    float MinY = Math::Max((Cmd.ClipRect.y - ClipOff.y) * ClipScale.y, 0.0f);
                    float MaxX = Math::Min((Cmd.ClipRect.z - ClipOff.x) * ClipScale.x, FBW);
                    float MaxY = Math::Min((Cmd.ClipRect.w - ClipOff.y) * ClipScale.y, FBH);
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

        // The surface is created here because the window-system calls are main-thread only.
        FImGuiViewportData* Data = IM_NEW(FImGuiViewportData)();
        Data->Window = Viewport->PlatformHandle;
        Data->Target.Initialize(RHI::CreateSurface(Data->Window));
        Viewport->RendererUserData = Data;
    }

    void FVulkanImGuiRender::OnRendererDestroyWindow(ImGuiViewport* Viewport)
    {
        FImGuiViewportData* Data = static_cast<FImGuiViewportData*>(Viewport->RendererUserData);
        if (Data == nullptr)
        {
            return;
        }

        Data->Target.Shutdown();

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

        Data->Target.Resize(FUIntVector2((uint32)ReqW, (uint32)ReqH));

        RHI::FTextureH Img = Data->Target.Acquire();
        if (!RHI::IsValid(Img))
        {
            return;   // minimized, mid-drag or stale; the retry is already armed
        }

        const FUIntVector2 ImgExtent = Data->Target.GetExtent();

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        Data->Target.BarrierToRender(CL);

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

        // The actual image can be smaller than requested when the surface clamped.
        RecordDrawLists(CL, DrawData, (float)ImgExtent.x, (float)ImgExtent.y);

        RHI::CmdEndRenderPass(CL);
        Data->Target.Present(CL);
    }

    ImTextureID FVulkanImGuiRender::GetOrCreateImTexture(FStringView Path)
    {
        FRecursiveScopeLock Lock(Mutex);

        const FName NamePath = Path;
        if (auto It = PathTextures.find(NamePath); It != PathTextures.end())
        {
            return (ImTextureID)(uint32)It->second.ResourceID();
        }

        // Decoded straight into the new heap, so the texture id is one the ImGui shaders sample directly.
        TOptional<Import::Textures::FTextureImportResult> Result = Import::Textures::ImportTexture(Path, false);
        if (!Result.has_value() || Result->Pixels.empty())
        {
            return (ImTextureID)(uint32)RHI::Textures::DefaultResourceID();
        }

        const FString DebugName = FString("ImGui.") + FString(Path.data(), Path.size());

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result->Dimensions.x,
            .Height = Result->Dimensions.y,
            .Format = Result->Format,
            .DebugName = DebugName.c_str(),
        });
        RHI::Textures::Upload(Texture, 0, Result->Pixels.data(), Result->Pixels.size(), Result->Dimensions.x);

        const uint32 ResourceID = Texture.ResourceID();
        PathTextures.insert_or_assign(NamePath, Move(Texture));
        return (ImTextureID)(uint32)ResourceID;
    }

}
