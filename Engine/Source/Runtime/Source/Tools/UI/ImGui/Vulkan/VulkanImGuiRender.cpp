#include "RuntimePCH.h"
#include "VulkanImGuiRender.h"

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"

#include "Core/Windows/GLFWInclude.h"
#include <algorithm>

#include "Core/Engine/Engine.h"
#include "Core/Profiler/Profile.h"
#include "Core/Windows/Window.h"
#include "Input/InputViewport.h"
#include "Memory/Memcpy.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/UI/ImGui/ImGuiNativeFrames.h"

namespace Lumina
{
    // The single live backend instance, so the static ImGuiPlatformIO::Renderer_* hooks can reach it.
    static FVulkanImGuiRender* GImGuiBackend = nullptr;

    // Mirrors FImGuiArgs in Includes/ImGuiCommon.slang, scalar layout.
    struct FImGuiArgs
    {
        float  Scale[2];
        float  Translate[2];
        float  ClipOffset[2];
        float  ClipScale[2];
        uint32 SamplerIndex;
        uint64 VertexAddr;
        uint64 DrawsAddr;
    };
    static_assert(sizeof(FImGuiArgs) == 56, "FImGuiArgs must match ImGuiCommon.slang.");

    // Mirrors FImGuiDraw in Includes/ImGuiCommon.slang, one per ImGui command.
    struct FImGuiDraw
    {
        float  ClipRect[4];
        uint32 TextureID;
        uint32 DisplayMode;
        float  Exposure;
        uint32 ArraySlice;
        uint32 bIsArray;
    };
    static_assert(sizeof(FImGuiDraw) == 36, "FImGuiDraw must match ImGuiCommon.slang.");

    void FVulkanImGuiRender::Initialize()
    {
        IImGuiRenderer::Initialize();
        LUMINA_PROFILE_SCOPE();

        ImGui_ImplGlfw_InitForVulkan(Windowing::GetPrimaryWindowHandle()->GetWindow(), true);
        if (!bOverlay)
        {
            ImGuiX::NativeFrames::Install();
        }

        ImGuiIO& IO = ImGui::GetIO();
        IO.BackendRendererName = "Lumina_RHI";
        IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // 16-bit indices + VtxOffset for >64K meshes
        IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // ImTextureData create/update/destroy path
        if (!bOverlay)
        {
            IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        }

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

        for (auto& [Key, Pipeline] : Pipelines)
        {
            RHI::Retire(Pipeline);
        }
        Pipelines.Reset();
        for (auto& [FontIndex, Texture] : NewFontTextures)
        {
            RHI::Textures::Release(Texture);
        }
        NewFontTextures.Reset();

        for (auto& [Path, Texture] : PathTextures)
        {
            RHI::Textures::Release(Texture);
        }
        PathTextures.Reset();

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
            const bool bGameCursor = FInputViewportRegistry::Get().IsGameCursorShowing();
            ImGuiIO& IO = ImGui::GetIO();
            if (bGameCursor && !(IO.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange))
            {
                IO.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
                bHoldingCursorChange = true;
            }
            else if (!bGameCursor && bHoldingCursorChange)
            {
                IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
                bHoldingCursorChange = false;
            }
        }
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

        bool bUploaded = false;

        // So the atlas texture id is a new-heap resource id the ImGui shaders sample directly.
        for (ImTextureData* Tex : ImGui::GetPlatformIO().Textures)
        {
            if (Tex->Status == ImTextureStatus_WantCreate || Tex->Status == ImTextureStatus_WantUpdates)
            {
                auto It = NewFontTextures.Find(Tex->UniqueID);
                if (It == nullptr)
                {
                    RHI::FManagedTexture Created = RHI::Textures::Create(RHI::FTexture2DDesc
                    {
                        .Width  = (uint32)Tex->Width,
                        .Height = (uint32)Tex->Height,
                        .Format = EFormat::RGBA8_UNORM,
                        .DebugName = "ImGui.FontAtlas",
                    });
                    It = &NewFontTextures.Add(Tex->UniqueID, Created);
                    Tex->SetTexID((ImTextureID)Created.ResourceID());
                }

                if (Tex->GetPixels() != nullptr)
                {
                    const uint64 Bytes = (uint64)Tex->Width * (uint64)Tex->Height * 4;
                    bUploaded |= RHI::Textures::Upload((*It), 0, Tex->GetPixels(), Bytes, (uint32)Tex->Width);
                }

                Tex->SetStatus(ImTextureStatus_OK);
            }
            else if (Tex->Status == ImTextureStatus_WantDestroy && Tex->UnusedFrames >= (RHI::kFramesInFlight * 2))
            {
                auto It = NewFontTextures.Find(Tex->UniqueID);
                if (It != nullptr)
                {
                    RHI::Textures::Release(*It);
                    NewFontTextures.Remove(Tex->UniqueID);
                }
                Tex->SetTexID(ImTextureID_Invalid);
                Tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        // This frame's upload flush already ran, and the atlas is sampled before the next one.
        if (bUploaded)
        {
            RHI::FlushUploads();
        }
    }

    void FVulkanImGuiRender::OnEndFrame(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, ImDrawData* DrawData)
    {
        RecordDrawData_NewRHI(CL, DrawData, Target, Extent);
    }

    RHI::FPipelineH FVulkanImGuiRender::GetPipeline(EFormat TargetFormat, bool bOpaque)
    {
        const uint64 Key = (uint64)TargetFormat | (bOpaque ? (1ull << 32) : 0ull);
        if (const RHI::FPipelineH* Found = Pipelines.Find(Key))
        {
            return *Found;
        }

        // Alpha accumulates coverage, which is what pulls the HDR gain under translucent UI back toward 1.
        RHI::FBlendDesc Blend;
        Blend.bBlendEnable   = !bOpaque;
        Blend.ColorOp        = RHI::EBlend::Add;
        Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        Blend.AlphaOp        = RHI::EBlend::Add;
        Blend.SrcAlphaFactor = RHI::EFactor::One;
        Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        const RHI::FColorTarget ColorTarget { .Format = TargetFormat, .Blend = Blend };
        RHI::FRasterDesc RasterDesc;
        RasterDesc.Topology     = RHI::ETopology::TriangleList;
        RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        const RHI::FPipelineH Pipeline = RHI::CreateGraphicsPipeline("ImGuiVert.slang", "ImGuiPixel.slang", RasterDesc);
        Pipelines.Add(Key, Pipeline);
        return Pipeline;
    }

    void FVulkanImGuiRender::RecordDrawData_NewRHI(RHI::FCmdListH CL, ImDrawData* DrawData, RHI::FTextureH Target, const FUIntVector2& Extent)
    {
        // Always begin/end the pass so the swapchain image is cleared even with no draws; an overlay keeps the game image under it.
        RHI::FRenderAttachment Color;
        Color.Texture  = Target;
        Color.LoadOp   = bOverlay ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;
        Color.Color[0] = Color.Color[1] = Color.Color[2] = 0.0f;
        Color.Color[3] = 1.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);

        RecordDrawLists(CL, DrawData, RHI::GetTextureDesc(Target).Format, (float)Extent.x, (float)Extent.y);

        RHI::CmdEndRenderPass(CL);
    }

    void FVulkanImGuiRender::RecordDrawLists(RHI::FCmdListH CL, ImDrawData* DrawData, EFormat TargetFormat, float FBW, float FBH)
    {
        const RHI::FPipelineH BlendedPipeline = GetPipeline(TargetFormat, false);
        if (DrawData == nullptr || DrawData->TotalVtxCount <= 0 || DrawData->TotalIdxCount <= 0 || FBW <= 0.0f || FBH <= 0.0f || !BlendedPipeline)
        {
            return;
        }

        int32 MaxDraws = 0;
        for (const ImDrawList* List : DrawData->CmdLists)
        {
            MaxDraws += List->CmdBuffer.Size;
        }

        RHI::FTransientAlloc VB       = RHI::AllocTransient((size_t)DrawData->TotalVtxCount * sizeof(ImDrawVert), 16);
        RHI::FTransientAlloc IB       = RHI::AllocTransient((size_t)DrawData->TotalIdxCount * sizeof(ImDrawIdx), 4);
        RHI::FTransientAlloc DrawRows = RHI::AllocTransient((size_t)MaxDraws * sizeof(FImGuiDraw), 16);
        RHI::FTransientAlloc Indirect = RHI::AllocTransient((size_t)MaxDraws * sizeof(RHI::FDrawIndexedIndirectArguments), 16);

        ImDrawVert* VtxDst = static_cast<ImDrawVert*>(VB.Cpu);
        ImDrawIdx*  IdxDst = static_cast<ImDrawIdx*>(IB.Cpu);
        for (const ImDrawList* List : DrawData->CmdLists)
        {
            Memory::Memcpy(VtxDst, List->VtxBuffer.Data, (size_t)List->VtxBuffer.Size * sizeof(ImDrawVert));
            Memory::Memcpy(IdxDst, List->IdxBuffer.Data, (size_t)List->IdxBuffer.Size * sizeof(ImDrawIdx));
            VtxDst += List->VtxBuffer.Size;
            IdxDst += List->IdxBuffer.Size;
        }

        const ImVec2 ClipOff   = DrawData->DisplayPos;
        const ImVec2 ClipScale = DrawData->FramebufferScale;

        FImGuiArgs Args;
        Args.Scale[0]      = 2.0f / DrawData->DisplaySize.x;
        Args.Scale[1]      = 2.0f / DrawData->DisplaySize.y;
        Args.Translate[0]  = -1.0f - ClipOff.x * Args.Scale[0];
        Args.Translate[1]  = -1.0f - ClipOff.y * Args.Scale[1];
        Args.ClipOffset[0] = ClipOff.x;
        Args.ClipOffset[1] = ClipOff.y;
        Args.ClipScale[0]  = ClipScale.x;
        Args.ClipScale[1]  = ClipScale.y;
        Args.SamplerIndex  = (uint32)RHI::EStockSampler::LinearWrap;
        Args.VertexAddr    = VB.Gpu;
        Args.DrawsAddr     = DrawRows.Gpu;
        const RHI::GPUPtr ArgsPtr = RHI::CopyTransient(Args);

        // UI atlases hold display-encoded bytes in plain Texture2Ds until a display state callback says otherwise.
        FImGuiDraw State = {};
        State.Exposure = 1.0f;

        RHI::CmdSetDepthStencil(CL, RHI::FDepthStencilDesc{});
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CCW);
        RHI::CmdSetScissor(CL, RHI::FRect{ 0, (int)FBW, 0, (int)FBH });
        RHI::CmdSetPipeline(CL, BlendedPipeline);
        RHI::FPipelineH BoundPipeline = BlendedPipeline;

        auto* Rows = static_cast<FImGuiDraw*>(DrawRows.Cpu);
        auto* Commands = static_cast<RHI::FDrawIndexedIndirectArguments*>(Indirect.Cpu);
        uint32 NumDraws = 0;
        uint32 FirstUnissued = 0;
        auto IssueDraws = [&]()
        {
            const uint32 Count = NumDraws - FirstUnissued;
            if (Count == 0)
            {
                return;
            }
            constexpr uint32 Stride = sizeof(RHI::FDrawIndexedIndirectArguments);
            const RHI::FGPURange Range { Indirect.Gpu + (uint64)FirstUnissued * Stride, (uint64)Count * Stride };
            RHI::CmdDrawIndexedIndirect(CL, IB, ArgsPtr, Range, Count, Stride, RHI::EIndexType::Uint16);
            FirstUnissued = NumDraws;
        };

        // Resolved once, since the preview stamps this marker and the draws match on identity.
        const ImDrawCallback DisplayStateCallback = ImGuiX::Detail::GetDisplayStateCallback();
        const uint32 DefaultTex = RHI::Textures::DefaultResourceID();

        uint32 GlobalVtx = 0, GlobalIdx = 0;
        for (const ImDrawList* List : DrawData->CmdLists)
        {
            for (const ImDrawCmd& Cmd : List->CmdBuffer)
            {
                if (Cmd.UserCallback != nullptr)
                {
                    // It carries no geometry, so it updates state for the draws that follow and contributes nothing.
                    if (Cmd.UserCallback == DisplayStateCallback &&
                        Cmd.UserCallbackData != nullptr &&
                        Cmd.UserCallbackDataSize == (int)sizeof(ImGuiX::Detail::FImGuiDisplayState))
                    {
                        const auto* Display = static_cast<const ImGuiX::Detail::FImGuiDisplayState*>(Cmd.UserCallbackData);
                        State.DisplayMode = Display->DisplayMode;
                        State.Exposure    = Display->Exposure;
                        State.ArraySlice  = Display->ArraySlice;
                        State.bIsArray    = Display->bIsArray;

                        const bool bOpaque = Display->DisplayMode == ImGuiX::Detail::GDisplayModeScene;
                        const RHI::FPipelineH Wanted = bOpaque ? GetPipeline(TargetFormat, true) : BlendedPipeline;
                        if (Wanted && Wanted != BoundPipeline)
                        {
                            IssueDraws();
                            RHI::CmdSetPipeline(CL, Wanted);
                            BoundPipeline = Wanted;
                        }
                    }
                    continue;
                }

                // Whole pixels, so the clip distances keep exactly the pixels a scissor of this rect would.
                const float MinX = Math::Floor(Math::Max((Cmd.ClipRect.x - ClipOff.x) * ClipScale.x, 0.0f));
                const float MinY = Math::Floor(Math::Max((Cmd.ClipRect.y - ClipOff.y) * ClipScale.y, 0.0f));
                const float MaxX = Math::Floor(Math::Min((Cmd.ClipRect.z - ClipOff.x) * ClipScale.x, FBW));
                const float MaxY = Math::Floor(Math::Min((Cmd.ClipRect.w - ClipOff.y) * ClipScale.y, FBH));
                if (MaxX <= MinX || MaxY <= MinY || Cmd.ElemCount == 0)
                {
                    continue;
                }

                const int32 RawTexID = (int32)Cmd.GetTexID();
                FImGuiDraw& Row = Rows[NumDraws];
                Row = State;
                Row.ClipRect[0] = MinX;
                Row.ClipRect[1] = MinY;
                Row.ClipRect[2] = MaxX;
                Row.ClipRect[3] = MaxY;
                Row.TextureID   = (RawTexID >= 0) ? (uint32)RawTexID : DefaultTex;

                Commands[NumDraws] = RHI::FDrawIndexedIndirectArguments
                {
                    .IndexCount    = Cmd.ElemCount,
                    .InstanceCount = 1,
                    .FirstIndex    = Cmd.IdxOffset + GlobalIdx,
                    .VertexOffset  = (int32)(Cmd.VtxOffset + GlobalVtx),
                    .FirstInstance = NumDraws,
                };
                ++NumDraws;
            }
            GlobalVtx += List->VtxBuffer.Size;
            GlobalIdx += List->IdxBuffer.Size;
        }
        IssueDraws();
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
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
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
        RecordDrawLists(CL, DrawData, Data->Target.GetFormat(), (float)ImgExtent.x, (float)ImgExtent.y);

        RHI::CmdEndRenderPass(CL);
        Data->Target.Present(CL);
    }

    ImTextureID FVulkanImGuiRender::GetOrCreateImTexture(FStringView Path)
    {
        FRecursiveScopeLock Lock(Mutex);

        const FName NamePath = Path;
        if (auto It = PathTextures.Find(NamePath); It != nullptr)
        {
            return (ImTextureID)It->ResourceID();
        }

        // Decoded straight into the new heap, so the texture id is one the ImGui shaders sample directly.
        TOptional<Import::Textures::FTextureImportResult> Result = Import::Textures::ImportTexture(Path, false);
        if (!Result.IsSet() || Result->Pixels.IsEmpty())
        {
            return (ImTextureID)RHI::Textures::DefaultResourceID();
        }

        const FString DebugName = FString("ImGui.") + FString(Path.GetData(), Path.Len());

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result->Dimensions.x,
            .Height = Result->Dimensions.y,
            .Format = Result->Format,
            .DebugName = *DebugName,
        });
        RHI::Textures::Upload(Texture, 0, Result->Pixels.GetData(), Result->Pixels.Num(), Result->Dimensions.x);

        const uint32 ResourceID = Texture.ResourceID();
        PathTextures.Add(NamePath, Move(Texture));
        return (ImTextureID)(uint32)ResourceID;
    }

}
