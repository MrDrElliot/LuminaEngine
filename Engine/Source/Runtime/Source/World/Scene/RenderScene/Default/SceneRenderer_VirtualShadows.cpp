#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererVirtualShadowInternal
    {
        // Mirrors the modes and counters of VirtualShadowPages.slang.
        constexpr uint32 kModeReset      = 0u;
        constexpr uint32 kModeInvalidate = 1u;
        constexpr uint32 kModeRelease    = 2u;
        constexpr uint32 kModeAllocate   = 3u;
        constexpr uint32 kModeFinish     = 4u;
        constexpr uint32 kModeMaskBase   = 5u;
        constexpr uint32 kModeMaskMip    = 6u;
        constexpr uint32 kModeInitialize = 7u;
        constexpr uint32 kModeCount      = 8u;

        constexpr uint32 kCounterSlots   = 9u;
        constexpr uint32 kFreeCount      = 0u;
        constexpr uint32 kStaticCount    = 2u;
        constexpr uint32 kRequestedCount = 3u;
        constexpr uint32 kAllocFailures  = 4u;
        // The static layer's count, with the dynamic layer's in the slot after it.
        constexpr uint32 kComputeRasterCount = 7u;
        // Below this many eligible meshlets the dispatches and barriers cost more than the hardware raster they replace.
        constexpr uint32 kMinComputeRasterMeshlets = 4096u;
        // Each camera meshlet saves about a nanosecond, so below this many the two fullscreen resolves cost more than they win back.
        constexpr uint32 kMinCameraRasterMeshlets = 1u << 17;

        // Texels past the filter's reach a pixel's request covers, for the normal offset the lighting adds.
        constexpr float  kMarkMarginTexels = 4.0f;

        struct FPagePassPC
        {
            RHI::TGPUSpan<FVirtualShadowPageGPU>  Pages;
            RHI::TGPUSpan<FVirtualShadowLevelGPU> Levels;
            RHI::TGPUSpan<uint32>                 Masks;
            RHI::TGPUSpan<uint32>                 FreeStack;
            RHI::TGPUSpan<uint32>                 Counters;
            RHI::TGPUSpan<uint32>                 StaticList;
            RHI::TGPUSpan<uint32>                 RequestedList;
            RHI::TGPUSpan<FVector4>               Spheres;
            RHI::TGPUSpan<FUIntVector4>           FillArgs;
            uint32                                Mode = 0u;
            uint32                                Frame = 0u;
            uint32                                PoolPages = 0u;
            uint32                                Mip = 0u;
        };
        static_assert(sizeof(FPagePassPC) == 160, "FPagePassPC must match VirtualShadowPages.slang.");

        struct FMarkPC
        {
            RHI::TGPUSpan<FVirtualShadowPageGPU> Pages;
            uint32                               Depth = 0u;
            uint32                               ScreenW = 0u;
            uint32                               ScreenH = 0u;
            uint32                               Frame = 0u;
            float                                MarginTexels = 0.0f;
            uint32                               _Pad0 = 0u;
            uint32                               _Pad1 = 0u;
            uint32                               _Pad2 = 0u;
        };
        static_assert(sizeof(FMarkPC) == 48, "FMarkPC must match VirtualShadowMark.slang.");

        struct FFillPC
        {
            RHI::TGPUSpan<FVirtualShadowPageGPU> Pages;
            RHI::TGPUSpan<uint32>                List;
            RHI::TGPUSpan<uint32>                Counters;
            uint32                               Pool = 0u;
            uint32                               CountSlot = 0u;
            uint32                               bCopy = 0u;
            uint32                               _Pad0 = 0u;
        };
        static_assert(sizeof(FFillPC) == 64, "FFillPC must match VirtualShadowFill.slang.");

        constexpr uint32 kTotalPages = VSM_MAX_LEVELS * VSM_PAGES_PER_LEVEL;

        // One thread per item, folded into rows past the dispatch axis limit as the page shader unfolds it.
        FUIntVector2 FoldedGroups(uint32 Items)
        {
            const uint32 Groups = Math::Max(RenderUtils::GetGroupCount(Items, VSM_PAGE_GROUP_SIZE), 1u);
            return FUIntVector2(Math::Min(Groups, (uint32)MAX_DISPATCH_AXIS), RenderUtils::GetGroupCount(Groups, (uint32)MAX_DISPATCH_AXIS));
        }

        void ComputeToCompute(RHI::FCmdListH CL)
        {
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
                            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
        }
    }

    bool FDefaultSceneRenderer::EnsureVirtualShadowResources(RHI::FCmdListH CL)
    {
        using namespace SceneRendererVirtualShadowInternal;

        auto& Res = VirtualShadowResources;
        const auto& Virtual = RenderFrame->Lighting.VirtualShadow;
        if (!Virtual.bActive || Virtual.PoolPages == 0u)
        {
            return false;
        }
        if (Res.bInitialized && Res.PoolPages == Virtual.PoolPages)
        {
            return true;
        }

        for (FSceneBuffer* Buffer : { &Res.Pages, &Res.Masks, &Res.FreeStack, &Res.Counters, &Res.StaticList, &Res.RequestedList, &Res.FillArgs,
                                      &Res.ComputeRasterList })
        {
            Buffer->Release();
        }
        RetireSceneImage(Res.Pool);

        const uint32 PoolPages = Virtual.PoolPages;
        const uint32 PoolRows  = PoolPages / VSM_POOL_PAGES_X;
        Res.Pages.Allocate((uint64)kTotalPages * sizeof(FVirtualShadowPageGPU));
        Res.Masks.Allocate((uint64)VSM_MAX_LEVELS * VSM_MASK_CELLS_PER_LEVEL * sizeof(uint32));
        Res.FreeStack.Allocate((uint64)PoolPages * sizeof(uint32));
        Res.Counters.Allocate((uint64)kCounterSlots * sizeof(uint32));
        Res.StaticList.Allocate((uint64)PoolPages * sizeof(uint32));
        Res.RequestedList.Allocate((uint64)PoolPages * sizeof(uint32));
        Res.FillArgs.Allocate(2ull * sizeof(FUIntVector4));
        Res.ComputeRasterList.Allocate(2ull * VSM_COMPUTE_RASTER_CAPACITY * sizeof(FUIntVector2));

        // The static layer's rows, then the dynamic layer's below them.
        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(VSM_POOL_PAGES_X * VSM_PAGE_SIZE, PoolRows * VSM_PAGE_SIZE * 2u, 1u);
        Desc.Format    = EFormat::R32_UINT;
        Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;
        Res.Pool = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);
        RHI::SetDebugName(Res.Pool.Texture, "VirtualShadow.Pool");
        if (!Res.Pages || !Res.Masks || !Res.FreeStack || !Res.Counters || !Res.Pool.IsValid())
        {
            return false;
        }

        // Empty masks keep the first frame's cull from reading garbage, and the page pass fills the table before anything else reads it.
        RHI::CmdMemset(CL, { Res.Masks.Gpu, Res.Masks.Size }, 0u);
        RHI::CmdMemset(CL, { Res.Counters.Gpu, Res.Counters.Size }, 0u);
        Barriers::TransferToCompute(CL);

        Res.PoolPages    = PoolPages;
        Res.bInitialized = true;
        Res.bNeedsTable  = true;
        return true;
    }

    void FDefaultSceneRenderer::GetVirtualShadowComputeRaster(RHI::FCmdListH CL, RHI::TGPUSpan<FUIntVector2>& OutList, RHI::TGPUSpan<uint32>& OutCursors,
                                                              RHI::TGPUSpan<uint32>& OutDebug)
    {
        using namespace SceneRendererVirtualShadowInternal;

        OutList    = {};
        OutCursors = {};
        OutDebug   = {};

        auto& Res = VirtualShadowResources;
        Res.bComputeRasterActive    = false;
        Res.bComputeRasterCounting  = false;
        Res.ComputeRasterDebugWords = 0u;

        // Recorded whether or not the compute raster runs, so the view shows hardware casters too.
        if (RenderFrame->SceneGlobalData.CullData.DebugMode == (uint32)ERenderSceneDebugFlags::ShadowRaster && Res.bInitialized)
        {
            const uint32 Words = 2u * FrameVisibleInstanceCapacity;
            ReserveBuffer(CL, Res.ComputeRasterDebug, (SIZE_T)Math::Max(Words, 2u) * sizeof(uint32));
            if (Res.ComputeRasterDebug && Res.ComputeRasterDebug.CapacityOf<uint32>() >= Words)
            {
                RHI::CmdMemset(CL, { Res.ComputeRasterDebug.Gpu, (uint64)Words * sizeof(uint32) }, 0u);
                Barriers::TransferToCompute(CL);
                Res.ComputeRasterDebugWords = Words;
                OutDebug = { Res.ComputeRasterDebug, Words };
            }
        }

        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        if (Settings == nullptr || !Settings->bVirtualShadowComputeRaster || !Res.bInitialized || !Res.ComputeRasterList
            || ComputeRasterBatchSpan.Count == 0u)
        {
            return;
        }

        // An inactive frame still counts its candidates through a list with no room, so demand keeps being measured.
        Res.bComputeRasterCounting = true;
        Res.bComputeRasterActive   = Res.ComputeRasterDemand >= kMinComputeRasterMeshlets;
        OutList    = { Res.ComputeRasterList, 2u * VSM_COMPUTE_RASTER_CAPACITY };
        OutList.Count = Res.bComputeRasterActive ? OutList.Count : 0u;
        OutCursors.Address = Res.Counters.Gpu + kComputeRasterCount * sizeof(uint32);
        OutCursors.Count   = 2u;
    }

    void FDefaultSceneRenderer::PrepareComputeRaster(RHI::FCmdListH CL)
    {
        using namespace SceneRendererVirtualShadowInternal;

        ComputeRasterBatchBits.Reset();
        ComputeRasterBatchSpan = {};
        bCameraComputeRasterCounting = false;
        bCameraComputeRaster         = false;

        const FFrameData& Frame = *RenderFrame;
        const uint32 NumDraws = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        ComputeRasterBatchBits.Init(0u, (int32)RenderUtils::GetGroupCount(NumDraws, 32u));
        bool bAny = false;
        for (uint32 OpaqueIdx : Frame.Geometry.OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = Frame.Geometry.DrawCommands[OpaqueIdx];
            const uint32 DrawID = Batch.IndirectDrawOffset;
            // The compute raster draws the bare mesh, so a caster that clips texels or moves vertices stays in hardware.
            if (Batch.bMasked || Batch.bWorldPositionOffset || DrawID >= NumDraws)
            {
                continue;
            }
            ComputeRasterBatchBits[DrawID >> 5u] |= 1u << (DrawID & 31u);
            bAny = true;
        }
        if (bAny)
        {
            ComputeRasterBatchSpan = RHI::CopyTransientArray(ComputeRasterBatchBits.GetData(), ComputeRasterBatchBits.Num());
        }

        // Wireframe draws edges the compute raster never produces, and a device without 64-bit buffer atomics cannot keep depth and ID together.
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);
        if (Settings == nullptr || !Settings->bVisBufferComputeRaster || !RHI::SupportsBufferInt64Atomics() || FrameSettings.bWireframe
            || !VisRT.IsValid() || Frame.Views.CullViews.IsEmpty())
        {
            return;
        }

        if (!bAny)
        {
            return;
        }
        ReserveBuffer(CL, CameraRasterCursors, 2u * sizeof(uint32));
        if (!CameraRasterCursors)
        {
            return;
        }

        // Last frame's eligible meshlets decide this frame, since the cull only learns how many there are by counting them.
        const FUIntVector2 Size = VisRT.GetExtent();
        if (LastCameraRasterDemand >= kMinCameraRasterMeshlets)
        {
            ReserveBuffer(CL, CameraRasterList, (uint64)VISBUFFER_COMPUTE_RASTER_CAPACITY * sizeof(uint32));
            ReserveBuffer(CL, CameraRasterTarget, (uint64)Size.x * Size.y * sizeof(uint64));
            bCameraComputeRaster = CameraRasterList && CameraRasterTarget.CapacityOf<uint64>() >= (uint64)Size.x * Size.y;
        }

        // Published by the transfer barrier that follows the bucket reset.
        RHI::CmdMemset(CL, { CameraRasterCursors.Gpu, 2u * sizeof(uint32) }, 0u);
        CameraRasterSize             = Size;
        bCameraComputeRasterCounting = true;
    }

    void FDefaultSceneRenderer::VirtualShadowPagePass(RHI::FCmdListH CL)
    {
        using namespace SceneRendererVirtualShadowInternal;

        const FFrameData& Frame = *RenderFrame;
        const auto& Virtual = Frame.Lighting.VirtualShadow;
        auto& Res = VirtualShadowResources;
        static const FShaderH PageShader = FShaderLibrary::Get("VirtualShadowPages.slang");
        static const FShaderH MarkShader = FShaderLibrary::Get("VirtualShadowMark.slang");
        static const FShaderH FillShader = FShaderLibrary::Get("VirtualShadowFill.slang");
        if (!Virtual.bActive || !PageShader || !MarkShader || !FillShader || !EnsureVirtualShadowResources(CL))
        {
            return;
        }

        const uint32 PoolPages = Res.PoolPages;

        // This slot's last frame finished on the GPU before the slot could be reused, so its counters are settled.
        RHI::FGPUAllocation& Readback = Res.StatsReadback[CurrentFrameSlot];
        if (Readback.Gpu == 0)
        {
            Readback = RHI::Malloc(sizeof(uint32) * kCounterSlots, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Readback.Gpu, "Readback.VirtualShadow");
            if (Readback.Cpu != nullptr)
            {
                Memory::Memzero(Readback.Cpu, sizeof(uint32) * kCounterSlots);
            }
        }
        else if (const uint32* Counters = Readback.CpuAs<const uint32>())
        {
            LUMINA_PROFILE_VALUE("Shadows/VirtualPagesRequested", (int64)Counters[kRequestedCount]);
            LUMINA_PROFILE_VALUE("Shadows/VirtualPagesDrawn",     (int64)Counters[kStaticCount]);
            LUMINA_PROFILE_VALUE("Shadows/VirtualPagesResident",  (int64)(PoolPages - Math::Min(Counters[kFreeCount], PoolPages)));
            LUMINA_PROFILE_VALUE("Shadows/VirtualPageFailures",   (int64)Counters[kAllocFailures]);
            Res.ComputeRasterDemand = Counters[kComputeRasterCount] + Counters[kComputeRasterCount + 1u];
            LUMINA_PROFILE_VALUE("Shadows/ComputeRasterMeshlets", (int64)Res.ComputeRasterDemand);
        }

        FPagePassPC PC;
        PC.Pages         = { Res.Pages, kTotalPages };
        PC.Levels        = RHI::CopyTransientArray(Virtual.Levels, VSM_MAX_LEVELS);
        PC.Masks         = { Res.Masks, VSM_MAX_LEVELS * VSM_MASK_CELLS_PER_LEVEL };
        PC.FreeStack     = { Res.FreeStack, PoolPages };
        PC.Counters      = { Res.Counters, kCounterSlots };
        PC.StaticList    = { Res.StaticList, PoolPages };
        PC.RequestedList = { Res.RequestedList, PoolPages };
        PC.FillArgs      = { Res.FillArgs, 2u };
        PC.Frame         = Virtual.Frame;
        PC.PoolPages     = PoolPages;

        const auto Run = [&](uint32 Mode, uint32 Items)
        {
            PC.Mode = Mode;
            const FUIntVector2 Groups = FoldedGroups(Items);
            DispatchCompute(CL, PageShader, PC, Groups.x, Groups.y);
            ComputeToCompute(CL);
        };

        if (Res.bNeedsTable)
        {
            Run(kModeInitialize, Math::Max(kTotalPages, PoolPages));
            Res.bNeedsTable = false;
        }
        Run(kModeReset, 1u);

        if (!Virtual.InvalidSpheres.IsEmpty())
        {
            PC.Spheres = RHI::CopyTransientArray(Virtual.InvalidSpheres.GetData(), Virtual.InvalidSpheres.Num());
            Run(kModeInvalidate, (uint32)Virtual.InvalidSpheres.Num() * VSM_MAX_LEVELS);
        }

        {
            const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);
            FMarkPC Mark;
            Mark.Pages        = PC.Pages;
            Mark.Depth        = (uint32)Depth.GetResourceID();
            Mark.ScreenW      = Depth.GetSizeX();
            Mark.ScreenH      = Depth.GetSizeY();
            Mark.Frame        = Virtual.Frame;
            Mark.MarginTexels = kMarkMarginTexels;
            DispatchCompute(CL, MarkShader, Mark, RenderUtils::GetGroupCount(Mark.ScreenW, VSM_MARK_GROUP_SIZE),
                            RenderUtils::GetGroupCount(Mark.ScreenH, VSM_MARK_GROUP_SIZE));
            ComputeToCompute(CL);
        }

        Run(kModeCount, kTotalPages);
        Run(kModeRelease, kTotalPages);
        Run(kModeAllocate, kTotalPages);
        Run(kModeFinish, 1u);
        Run(kModeMaskBase, kTotalPages);
        for (uint32 Mip = 1u; Mip < VSM_MASK_MIPS; ++Mip)
        {
            PC.Mip = Mip;
            const uint32 Side = VSM_LEVEL_PAGES >> Mip;
            Run(kModeMaskMip, VSM_MAX_LEVELS * Side * Side);
        }

        // Fresh and invalidated static pages start at the far plane before the raster fills them.
        FFillPC Fill;
        Fill.Pages     = PC.Pages;
        Fill.List      = PC.StaticList;
        Fill.Counters  = PC.Counters;
        Fill.Pool      = (uint32)Res.Pool.GetMipUAVIndex(0);
        Fill.CountSlot = kStaticCount;
        DispatchComputeIndirect(CL, FillShader, Fill, RHI::FGPURange{ Res.FillArgs.Gpu, sizeof(FUIntVector4) });
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferRead);
        RHI::CmdMemcpy(CL, { Readback.Gpu, sizeof(uint32) * kCounterSlots }, { Res.Counters.Gpu, sizeof(uint32) * kCounterSlots });
    }

    void FDefaultSceneRenderer::VirtualShadowRasterPass(RHI::FCmdListH CL)
    {
        using namespace SceneRendererVirtualShadowInternal;

        const FFrameData& Frame = *RenderFrame;
        const auto& Virtual = Frame.Lighting.VirtualShadow;
        auto& Res = VirtualShadowResources;
        static const FShaderH PixelShader = FShaderLibrary::Get("VirtualShadowPixel.slang");
        static const FShaderH FillShader  = FShaderLibrary::Get("VirtualShadowFill.slang");
        if (!Virtual.bActive || !Res.bInitialized || Virtual.ViewBase == Constants::kIndexNoneU32 || !PixelShader || !FillShader
            || Virtual.ShadowDataBase == Constants::kIndexNone)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Virtual Shadows");

        static const FShaderH ComputeRasterShader = FShaderLibrary::Get("VirtualShadowComputeRaster.slang");
        const bool bComputeRaster = ComputeRasterShader && Res.bComputeRasterActive;
        const auto ComputeRaster = [&](bool bStatic)
        {
            struct FComputeRasterPC
            {
                RHI::TGPUSpan<FUIntVector2> List;
                RHI::TGPUSpan<uint32>       Cursors;
                uint32                      bStatic = 0u;
                uint32                      _Pad0 = 0u;
                uint32                      _Pad1 = 0u;
                uint32                      _Pad2 = 0u;
            } PC;
            static_assert(sizeof(FComputeRasterPC) == 48, "FComputeRasterPC must match VirtualShadowComputeRaster.slang.");

            SCENE_GPU_SCOPE(CL, bStatic ? "Compute Raster Static" : "Compute Raster Dynamic");
            PC.List            = { Res.ComputeRasterList, 2u * VSM_COMPUTE_RASTER_CAPACITY };
            PC.Cursors.Address = Res.Counters.Gpu + kComputeRasterCount * sizeof(uint32);
            PC.Cursors.Count   = 2u;
            PC.bStatic        = bStatic ? 1u : 0u;
            DispatchCompute(CL, ComputeRasterShader, PC, VSM_COMPUTE_RASTER_GROUPS, 1u, 1u);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        };

        const RHI::FRect   Full{ 0, (int32)VSM_VIRTUAL_SIZE, 0, (int32)VSM_VIRTUAL_SIZE };
        const FUIntVector2 Extent(VSM_VIRTUAL_SIZE, VSM_VIRTUAL_SIZE);

        // A compute pass, so it lists the point casters before the static layer's render pass opens.
        CullImpostorShadows(CL);

        // The static views draw into the static layer, then the requested pages copy it into the dynamic layer the movers draw over.
        for (uint32 Split = 0u; Split < 2u; ++Split)
        {
            FShadowViewRun& ViewRun = ShadowViewRunScratch;
            ViewRun.Reset();
            for (uint32 Level = Virtual.FirstLevel; Level <= Virtual.LastLevel; ++Level)
            {
                const uint32 View = Virtual.ViewBase + Split * Virtual.NumLevelViews + (Level - Virtual.FirstLevel);
                ViewRun.Add(View, Virtual.ShadowDataBase + (int32)(Level / 6u), (int32)(Level % 6u), Full);
            }

            RHI::FRenderPassDesc Pass;
            Pass.RenderArea = Extent;
            SCENE_GPU_SCOPE(CL, Split == 0u ? "Static Layer" : "Dynamic Layer");
            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdSetDepthStencil(CL, RHI::FDepthStencilDesc{});

            if (!DrawShadowViewsTogether(CL, ViewRun, PixelShader, EFormat::UNKNOWN))
            {
                for (uint32 OpaqueIdx : Frame.Geometry.OpaqueDrawList)
                {
                    const FMeshDrawCommand& Batch = Frame.Geometry.DrawCommands[OpaqueIdx];
                    const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader, nullptr, EFormat::UNKNOWN);
                    for (int32 v = 0; v < ViewRun.Views.Num(); ++v)
                    {
                        RHI::CmdSetViewport(CL, Full);
                        RHI::CmdSetScissor(CL, Full);
                        DrawShadowBatch(CL, Batch, bUseMesh, ViewRun.FirstCullView + (uint32)v, ViewRun.Views[v].ShadowDataIndex,
                                        ViewRun.Views[v].ViewIndex, Extent);
                    }
                }
            }
            // Particles move every frame, so they draw only over the dynamic layer, while impostor points never move.
            if (Split == 1u)
            {
                ParticleShadowCasters(CL, Virtual.ShadowDataBase, Virtual.FirstLevel, Virtual.LastLevel);
            }
            else
            {
                DrawImpostorShadows(CL);
            }
            RHI::CmdEndRenderPass(CL);

            if (Split == 0u)
            {
                RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite,
                                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
                if (bComputeRaster)
                {
                    ComputeRaster(/*bStatic*/ true);
                }
                SCENE_GPU_SCOPE(CL, "Copy Static Pages");
                FFillPC Fill;
                Fill.Pages     = { Res.Pages, kTotalPages };
                Fill.List      = { Res.RequestedList, Res.PoolPages };
                Fill.Counters  = { Res.Counters, kCounterSlots };
                Fill.Pool      = (uint32)Res.Pool.GetMipUAVIndex(0);
                Fill.CountSlot = kRequestedCount;
                Fill.bCopy     = 1u;
                DispatchComputeIndirect(CL, FillShader, Fill, RHI::FGPURange{ Res.FillArgs.Gpu + sizeof(FUIntVector4), sizeof(FUIntVector4) });
                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                                RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
            else
            {
                if (bComputeRaster)
                {
                    RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite,
                                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
                    ComputeRaster(/*bStatic*/ false);
                }

                // The page pass read the counters back before the cull filled these, and an inactive frame still needs its demand.
                if (Res.bComputeRasterCounting)
                {
                    const uint64 CountOffset = kComputeRasterCount * sizeof(uint32);
                    RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
                    RHI::CmdMemcpy(CL, { Res.StatsReadback[CurrentFrameSlot].Gpu + CountOffset, 2u * sizeof(uint32) },
                                   { Res.Counters.Gpu + CountOffset, 2u * sizeof(uint32) });
                }
            }
        }
        Barriers::RasterToRead(CL);
    }
}
