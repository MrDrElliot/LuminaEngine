#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    static constexpr uint32 GPrefilterSampleCount = 256;

    struct FPrefilterPC
    {
        uint32 SrcCubeSRV     = 0;
        uint32 OutMipUAV      = 0;
        float  Roughness      = 0.0f;
        uint32 NumSamples     = 0;
        uint32 DstLayerOffset = 0;
        uint32 _Pad0          = 0;
        uint32 _Pad1          = 0;
        uint32 _Pad2          = 0;
    };
    static_assert(sizeof(FPrefilterPC) == 32, "FPrefilterPC must match PrefilterEnvMap.slang::FPushConstants.");

    void FDefaultSceneRenderer::ReflectionProbeBakePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& Bake = Frame.ReflectionProbes;

        if (Bake.BakingProbe < 0 || Bake.BakeViewIndex <= 0 || Bake.BakeViewIndex >= (int32)SceneViews.size())
        {
            if (Bake.BakingProbe >= 0)
            {
                LOG_WARN("[Probe] Bake SKIPPED: probe={} viewIndex={} numSceneViews={}",
                         Bake.BakingProbe, Bake.BakeViewIndex, (int32)SceneViews.size());
            }
            return;
        }

        const FSceneImage& CaptureCube = NamedImages[(int)ENamedImage::ProbeCaptureCube];
        const FSceneImage& ProbeArray  = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!CaptureCube.IsValid() || !ProbeArray.IsValid())
        {
            LOG_WARN("[Probe] Bake SKIPPED: captureCube={} probeArray={} (targets not allocated)",
                     CaptureCube.IsValid() ? 1 : 0, ProbeArray.IsValid() ? 1 : 0);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Reflection Probe Bake", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Reflection Probe Bake");

        FSceneView& View = SceneViews[Bake.BakeViewIndex];

        bCapturingProbe = true;

        const uint32 FaceSize = Bake.BakeFaceSize;

        const bool     bClearToColor = Bake.Captures[Bake.BakingProbe].bClearToColor;
        const FVector3 ClearRGB      = Bake.Captures[Bake.BakingProbe].ClearColor;

        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (Bake.FaceCullViews[Face] == ~0u)
            {
                continue;
            }

            PointAtView(View);
            CurrentCameraEarlyView = Bake.FaceCullViews[Face];

            SetSceneRoot(CL, View,
                RHI::CopyTransient(MakeSecondaryViewGlobals(Bake.FaceGlobals[Face])));

            VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
            TerrainCullPass(CL);
            TerrainDepthPrePass(CL);
            ClusterBuildPass(CL);
            LightCullPass(CL);

            if (bClearToColor)
            {
                const FSceneImage& ClearRT = GetNamedImage(ENamedImage::HDR);

                RHI::FRenderAttachment ClearColorAttachment;
                ClearColorAttachment.Texture        = ClearRT.Texture;
                ClearColorAttachment.LoadOp         = RHI::ELoadOp::Clear;
                ClearColorAttachment.StoreOp        = RHI::EStoreOp::Store;
                ClearColorAttachment.Color[0]       = ClearRGB.x;
                ClearColorAttachment.Color[1]       = ClearRGB.y;
                ClearColorAttachment.Color[2]       = ClearRGB.z;
                ClearColorAttachment.Color[3]       = 1.0f;

                RHI::FRenderPassDesc ClearPass;
                ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&ClearColorAttachment, 1);
                ClearPass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

                RHI::CmdBeginRenderPass(CL, ClearPass);
                RHI::CmdEndRenderPass(CL);
                Barriers::RasterToRead(CL);
            }
            else
            {
                EnvironmentPass(CL);
            }

            DecalPass(CL);
            ShadowMaskPass(CL);
            VisBufferClassifyPass(CL);
            MaterialGBufferPass(CL);
            DeferredLightingPass(CL);
            TerrainRenderPass(CL);

            const FSceneImage& FaceColor = GetNamedImage(ENamedImage::HDR);

            RHI::FTextureSlice SrcSlice;
            SrcSlice.Mip        = 0;
            SrcSlice.Layer      = 0;
            SrcSlice.LayerCount = 1;
            SrcSlice.Extent     = FUIntVector3(FaceSize, FaceSize, 1);

            RHI::FTextureSlice DstSlice = SrcSlice;
            DstSlice.Layer = (uint32)Face;

            RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTexture(CL, FaceColor.Texture, SrcSlice, CaptureCube.Texture, DstSlice);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Compute);
        }

        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
            if (ComputeShader != nullptr)
            {
                SCENE_GPU_SCOPE(CL, "Probe Prefilter");
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

                const uint32 NumMips      = ProbeArray.GetNumMips();
                const uint32 BaseFaceSize = ProbeArray.GetSizeX();
                constexpr uint32 PrefilterTile = 8u;

                for (uint32 Mip = 0; Mip < NumMips; ++Mip)
                {
                    FPrefilterPC PC = {};
                    PC.SrcCubeSRV     = (uint32)CaptureCube.GetResourceID();
                    PC.OutMipUAV      = (uint32)ProbeArray.GetMipUAVIndex(Mip);
                    PC.Roughness      = (NumMips <= 1u) ? 0.0f : (float)Mip / (float)(NumMips - 1u);
                    PC.NumSamples     = GPrefilterSampleCount;
                    PC.DstLayerOffset = (uint32)Bake.BakingProbe * 6u;

                    const uint32 MipFaceSize = std::max<uint32>(BaseFaceSize >> Mip, 1u);
                    const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
                    RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
                }

                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
            }
        }

        bCapturingProbe = false;

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;

        CompletedProbeBakes.fetch_or(1u << (uint32)Bake.BakingProbe, std::memory_order_acq_rel);
    }

    void FDefaultSceneRenderer::ClusterBuildPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cluster Build Pass", tracy::Color::Pink2);
        
        static const FShaderH ComputeShader = FShaderLibrary::Get("ClusterBuild.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 ClusterBuildGroupSize = 64;
        constexpr uint32 ClusterDispatchGroups = (NumClusters + ClusterBuildGroupSize - 1) / ClusterBuildGroupSize;
        RHI::CmdDispatch(CL, MakeArgs(), ClusterDispatchGroups, 1, 1);

        // LightCull consumes the cluster AABBs next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::LightCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderH ComputeShader = FShaderLibrary::Get("LightCull.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 LightCullGroupSize = 128;
        constexpr uint32 LightCullGroups    = (NumClusters + LightCullGroupSize - 1) / LightCullGroupSize;
        RHI::CmdDispatch(CL, MakeArgs(), LightCullGroups, 1, 1);

        // Cluster light lists feed the lit pixel shaders.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    static constexpr uint32 GMaterialMaxSlots = MATERIAL_MAX_SLOTS;

    // One allocation means one barrier between the three classify dispatches instead of three.
    // Indirect-argument offsets need only 4-byte alignment; 16 costs nothing and keeps the triples tidy.
    static uint32 AlignClassifyRegion(uint32 Offset) { return (Offset + 15u) & ~15u; }

    bool FDefaultSceneRenderer::BuildDeferredMaterialBinning(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        MaterialClassifyLayout = FMaterialClassifyLayout{};

        const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();
        if (Extent.x == 0u || Extent.y == 0u)
        {
            return false;
        }

        // The pixel list packs 16 bits per axis, so refuse rather than alias two pixels onto one.
        if (Extent.x > 0xFFFFu || Extent.y > 0xFFFFu)
        {
            static bool bWarnedExtent = false;
            if (!bWarnedExtent)
            {
                bWarnedExtent = true;
                LOG_WARN("Render extent {}x{} exceeds the 16-bit pixel-list packing; deferred shading is disabled for this view.",
                    Extent.x, Extent.y);
            }
            return false;
        }

        const auto& DeferredMaterials = Frame.Geometry.DeferredMaterials;

        BinnedDeferredSlotShaders.clear();
        BinnedDeferredSlotLookup.clear();
        uint32 MaxMaterialIndex = 0u;
        for (const auto& M : DeferredMaterials)
        {
            if (M.DeferredShader)
            {
                MaxMaterialIndex = Math::Max(MaxMaterialIndex, M.MaterialIndex);
            }
        }

        // Overflow shades through the default material rather than not at all. It reads the overflowed
        // material's own uniforms, so the surface is wrong but lit, which reads as a bug instead of a hole.
        CMaterial* DefaultMaterial = CMaterial::GetDefaultMaterial();
        const FShaderH FallbackShader = IsValid(DefaultMaterial) ? DefaultMaterial->GetDeferredShader() : FShaderH{};

        // One slot held back for the fallback, so a frame that fills the table can still claim it.
        const uint32 SlotBudget = FallbackShader ? (GMaterialMaxSlots - 1u) : GMaterialMaxSlots;

        BinnedDeferredSlotByMaterial.assign((size_t)MaxMaterialIndex + 1u, 0xFFFFFFFFu);
        for (const auto& M : DeferredMaterials)
        {
            if (!M.DeferredShader)
            {
                continue;
            }

            // Looked up rather than scanned; a scan cost visible materials times distinct shaders.
            uint32 Slot = 0xFFFFFFFFu;
            if (auto It = BinnedDeferredSlotLookup.find(M.DeferredShader.Handle); It != BinnedDeferredSlotLookup.end())
            {
                Slot = It->second;
            }

            if (Slot == 0xFFFFFFFFu)
            {
                const bool bFull = (uint32)BinnedDeferredSlotShaders.size() >= SlotBudget;
                if (bFull)
                {
                    static bool bWarnedSlotCap = false;
                    if (!bWarnedSlotCap)
                    {
                        bWarnedSlotCap = true;
                        LOG_WARN("More than {} distinct deferred material shaders are visible; the excess {}.",
                            SlotBudget, FallbackShader ? "shades as the default material" : "will not shade");
                    }

                    if (!FallbackShader)
                    {
                        continue;
                    }
                }

                const FShaderH BinShader = bFull ? FallbackShader : M.DeferredShader;

                // The fallback may already own a bin, either from a visible default material or an
                // earlier overflow, in which case it costs no extra slot.
                if (auto Found = BinnedDeferredSlotLookup.find(BinShader.Handle); Found != BinnedDeferredSlotLookup.end())
                {
                    Slot = Found->second;
                }
                else
                {
                    Slot = (uint32)BinnedDeferredSlotShaders.size();
                    BinnedDeferredSlotShaders.push_back(BinShader);
                    BinnedDeferredSlotLookup.emplace(BinShader.Handle, Slot);
                }
            }

            BinnedDeferredSlotByMaterial[M.MaterialIndex] = Slot;
        }

        const uint32 NumSlots = (uint32)BinnedDeferredSlotShaders.size();
        if (NumSlots == 0u)
        {
            return false;
        }

        const uint64 PixelListSize = (uint64)Extent.x * (uint64)Extent.y * sizeof(uint32);

        // Built into a local so a bail-out below leaves the member zeroed rather than half-filled.
        FMaterialClassifyLayout Layout;
        Layout.NumSlots = NumSlots;

        uint32 Cursor = 0u;
        Layout.CountsOffset  = Cursor; Cursor += NumSlots * (uint32)sizeof(uint32);
        Layout.StartsOffset  = Cursor; Cursor += NumSlots * (uint32)sizeof(uint32);
        Layout.CursorsOffset = Cursor; Cursor += NumSlots * (uint32)sizeof(uint32);
        Layout.TotalOffset   = Cursor; Cursor += (uint32)sizeof(uint32);

        Cursor = AlignClassifyRegion(Cursor);
        Layout.MaterialArgsOffset = Cursor;
        Cursor += NumSlots * (uint32)sizeof(RHI::FDispatchIndirectArguments);

        Cursor = AlignClassifyRegion(Cursor);
        Layout.LightArgsOffset = Cursor;
        Cursor += (uint32)sizeof(RHI::FDispatchIndirectArguments);

        Layout.BlockSize = AlignClassifyRegion(Cursor);

        ResizeBufferIfNeeded(CL, MaterialClassifyRing[CurrentFrameSlot], Layout.BlockSize, 1.0f,
                             MaterialClassifyRingLowUsage[CurrentFrameSlot], /*bAllowShrink*/ false,
                             EBufferInit::Undefined, "Material.ClassifyBlock");
        ResizeBufferIfNeeded(CL, MaterialPixelListRing[CurrentFrameSlot], PixelListSize, 1.2f,
                             MaterialPixelListRingLowUsage[CurrentFrameSlot], true, EBufferInit::Undefined,
                             "Material.PixelList");

        if (!GetMaterialClassify() || !GetMaterialPixelList())
        {
            return false;
        }

        Layout.ScreenW = Extent.x;
        Layout.ScreenH = Extent.y;
        // From the allocation, not from what was asked for, since the scatter bounds writes on this.
        Layout.PixelCapacity = (uint32)Math::Min<uint64>(
            GetMaterialPixelList().Size / sizeof(uint32), 0xFFFFFFFFull);

        MaterialClassifyLayout = Layout;
        return true;
    }

    // Counts, prefix-sums and scatters, and emits the indirect args so the CPU never learns them.
    void FDefaultSceneRenderer::VisBufferClassifyPass(RHI::FCmdListH CL)
    {
        MaterialClassifyLayout = FMaterialClassifyLayout{};

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Classify", tracy::Color::Orange3);

        static const FShaderH CountCS = FShaderLibrary::Get("VisBufferMaterialCount.slang");
        static const FShaderH PrefixCS = FShaderLibrary::Get("VisBufferMaterialPrefixSum.slang");
        static const FShaderH ScatterCS = FShaderLibrary::Get("VisBufferMaterialScatter.slang");
        if (!CountCS || !PrefixCS || !ScatterCS)
        {
            return;
        }

        if (!BuildDeferredMaterialBinning(CL))
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "VisBuffer Classify");

        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;

        const FSceneImage& VisRT    = GetNamedImage(ENamedImage::VisBuffer);
        const RHI::FGPUAllocation Classify = GetMaterialClassify();
        const RHI::FGPUAllocation PixelList = GetMaterialPixelList();

        const RHI::GPUPtr Base        = Classify.Gpu;
        const RHI::GPUPtr CountsAddr  = Base + Layout.CountsOffset;
        const RHI::GPUPtr StartsAddr  = Base + Layout.StartsOffset;
        const RHI::GPUPtr CursorsAddr = Base + Layout.CursorsOffset;
        const RHI::GPUPtr TotalAddr   = Base + Layout.TotalOffset;
        const RHI::GPUPtr MatArgsAddr = Base + Layout.MaterialArgsOffset;
        const RHI::GPUPtr LitArgsAddr = Base + Layout.LightArgsOffset;

        // MaterialIndex -> dense slot; uploaded to the transient ring and read by device address.
        const RHI::FGPURange SlotByMaterialRange =
            RHI::CopyTransientArray(BinnedDeferredSlotByMaterial.data(), BinnedDeferredSlotByMaterial.size());

        // Only the counters are cleared; the prefix sum below rewrites every other field.
        RHI::CmdMemset(CL, { CountsAddr, sizeof(uint32) * Layout.NumSlots }, 0u);
        Barriers::TransferToCompute(CL);

        const uint32 GroupsX = RenderUtils::GetGroupCount(Layout.ScreenW, (uint32)MATERIAL_CLASSIFY_TILE);
        const uint32 GroupsY = RenderUtils::GetGroupCount(Layout.ScreenH, (uint32)MATERIAL_CLASSIFY_TILE);

        const RHI::TGPUSpan<uint32> CountsSpan  = RHI::TGPUSpan<uint32>::FromAddress(CountsAddr, Layout.NumSlots);
        const RHI::TGPUSpan<uint32> StartsSpan  = RHI::TGPUSpan<uint32>::FromAddress(StartsAddr, Layout.NumSlots);
        const RHI::TGPUSpan<uint32> CursorsSpan = RHI::TGPUSpan<uint32>::FromAddress(CursorsAddr, Layout.NumSlots);
        const RHI::TGPUSpan<uint32> SlotByMaterialSpan = SlotByMaterialRange;

        struct FMaterialCountPC
        {
            RHI::TGPUSpan<uint32> Counts;
            RHI::TGPUSpan<uint32> SlotByMaterial;
            uint32      VisBufferIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      DrawListCount;
        } CountPC = {};
        static_assert(sizeof(FMaterialCountPC) == 48, "FMaterialCountPC must match VisBufferMaterialCount.slang FMaterialCountArgs.");
        CountPC.Counts              = CountsSpan;
        CountPC.SlotByMaterial      = SlotByMaterialSpan;
        CountPC.VisBufferIndex      = (uint32)VisRT.GetResourceID();
        CountPC.ScreenW             = Layout.ScreenW;
        CountPC.ScreenH             = Layout.ScreenH;
        CountPC.DrawListCount       = DrawListCapacity;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CountCS));
        RHI::CmdDispatch(CL, MakeArgs(CountPC), GroupsX, GroupsY, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

        struct FPrefixSumPC
        {
            RHI::TGPUSpan<uint32> Counts;
            RHI::TGPUSpan<uint32> Starts;
            RHI::TGPUSpan<uint32> Cursors;
            RHI::TGPUSpan<uint32> Args;
            RHI::TGPUSpan<uint32> LightArgs;
            RHI::TGPUSpan<uint32> Total;
        } PrefixPC = {};
        static_assert(sizeof(FPrefixSumPC) == 96, "FPrefixSumPC must match VisBufferMaterialPrefixSum.slang FPrefixSumArgs.");
        PrefixPC.Counts    = CountsSpan;
        PrefixPC.Starts    = StartsSpan;
        PrefixPC.Cursors   = CursorsSpan;
        PrefixPC.Args      = RHI::TGPUSpan<uint32>::FromAddress(MatArgsAddr, Layout.NumSlots * 3u);
        PrefixPC.LightArgs = RHI::TGPUSpan<uint32>::FromAddress(LitArgsAddr, 3u);
        PrefixPC.Total     = RHI::TGPUSpan<uint32>::FromAddress(TotalAddr, 1u);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(PrefixCS));
        RHI::CmdDispatch(CL, MakeArgs(PrefixPC), 1u, 1u, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

        struct FMaterialScatterPC
        {
            RHI::TGPUSpan<uint32> Cursors;
            RHI::TGPUSpan<uint32> PixelList;
            RHI::TGPUSpan<uint32> SlotByMaterial;
            uint32      VisBufferIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      DrawListCount;
        } ScatterPC = {};
        static_assert(sizeof(FMaterialScatterPC) == 64, "FMaterialScatterPC must match VisBufferMaterialScatter.slang FMaterialScatterArgs.");
        ScatterPC.Cursors        = CursorsSpan;
        ScatterPC.PixelList      = { PixelList, Layout.PixelCapacity };
        ScatterPC.SlotByMaterial = SlotByMaterialSpan;
        ScatterPC.VisBufferIndex = CountPC.VisBufferIndex;
        ScatterPC.ScreenW        = Layout.ScreenW;
        ScatterPC.ScreenH        = Layout.ScreenH;
        ScatterPC.DrawListCount  = DrawListCapacity;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScatterCS));
        RHI::CmdDispatch(CL, MakeArgs(ScatterPC), GroupsX, GroupsY, 1u);

        // The pixel list feeds the material dispatches; the argument triples feed the indirect fetch.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments);
    }

    // Compute, not a rasterized quad, since a pixel shader runs the graph 4x on a 1-pixel tri.
    void FDefaultSceneRenderer::MaterialGBufferPass(RHI::FCmdListH CL)
    {
        // Set by VisBufferClassifyPass, which runs immediately before this and zeroes it on any bail-out.
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Material GBuffer Pass", tracy::Color::Red);
        SCENE_GPU_SCOPE(CL, "Material GBuffer");

        const FFrameData& Frame  = *RenderFrame;
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        const RHI::FGPUAllocation Classify  = GetMaterialClassify();
        const RHI::FGPUAllocation PixelList = GetMaterialPixelList();
        const RHI::GPUPtr  Base      = Classify.Gpu;

        struct FDeferredMaterialPC
        {
            uint32      VisBufferIndex;
            uint32      DBufferAIndex;
            uint32      DBufferBIndex;
            uint32      DBufferCIndex;
            uint32      DrawListCount;
            uint32      SlotIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      GBufferAUAV;
            uint32      GBufferBUAV;
            uint32      GBufferCUAV;
            uint32      GBufferDUAV;
            uint32      VelocityUAV;
            uint32      _PadVelocity;
            RHI::TGPUSpan<uint32> PixelList;
            RHI::TGPUSpan<uint32> Starts;
            RHI::TGPUSpan<uint32> Counts;
        } PC = {};
        static_assert(sizeof(FDeferredMaterialPC) == 104, "FDeferredMaterialPC must match DeferredMaterial.slang FDeferredMaterialArgs.");

        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        if (Frame.Primitives.DecalExtracts.empty())
        {
            PC.DBufferAIndex = 0xFFFFFFFFu;
            PC.DBufferBIndex = 0xFFFFFFFFu;
            PC.DBufferCIndex = 0xFFFFFFFFu;
        }
        else
        {
            PC.DBufferAIndex = (uint32)GetNamedImage(ENamedImage::DBufferA).GetResourceID();
            PC.DBufferBIndex = (uint32)GetNamedImage(ENamedImage::DBufferB).GetResourceID();
            PC.DBufferCIndex = (uint32)GetNamedImage(ENamedImage::DBufferC).GetResourceID();
        }
        PC.DrawListCount = DrawListCapacity;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;

        const int32 UAVA = GetNamedImage(ENamedImage::GBufferA).GetMipUAVIndex(0);
        const int32 UAVB = GetNamedImage(ENamedImage::GBufferB).GetMipUAVIndex(0);
        const int32 UAVC = GetNamedImage(ENamedImage::GBufferC).GetMipUAVIndex(0);
        const int32 UAVD = GetNamedImage(ENamedImage::GBufferD).GetMipUAVIndex(0);
        if (UAVA < 0 || UAVB < 0 || UAVC < 0 || UAVD < 0)
        {
            LOG_ERROR("Deferred material pass: a GBuffer target has no storage heap slot; skipping the pass.");
            return;
        }
        PC.GBufferAUAV = (uint32)UAVA;
        PC.GBufferBUAV = (uint32)UAVB;
        PC.GBufferCUAV = (uint32)UAVC;
        PC.GBufferDUAV = (uint32)UAVD;

        PC.PixelList = { PixelList, Layout.PixelCapacity };
        PC.Starts    = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.StartsOffset, Layout.NumSlots);
        PC.Counts    = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.CountsOffset, Layout.NumSlots);

        // Invalid disables the write, which leaves the camera-only base the fullscreen pass laid down.
        PC.VelocityUAV = 0xFFFFFFFFu;
        if (IsVelocityWanted())
        {
            const int32 UAVVel = GetNamedImage(ENamedImage::Velocity).GetMipUAVIndex(0);
            if (UAVVel >= 0)
            {
                PC.VelocityUAV = (uint32)UAVVel;
            }
        }

        // One allocation written in place, since the blocks differ only in SlotIndex.
        const RHI::FTransientAlloc ArgsAlloc =
            RHI::AllocTransient(sizeof(FDeferredMaterialPC) * Layout.NumSlots);
        if (ArgsAlloc.Cpu == nullptr)
        {
            LOG_ERROR("Deferred material pass: could not stage {} argument blocks; skipping the pass.",
                Layout.NumSlots);
            return;
        }

        FDeferredMaterialPC* ArgsCpu = (FDeferredMaterialPC*)ArgsAlloc.Cpu;
        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            ArgsCpu[Slot]           = PC;
            ArgsCpu[Slot].SlotIndex = Slot;
        }

        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BinnedDeferredSlotShaders[Slot]));
            RHI::CmdDispatchIndirect(CL, ArgsAlloc.Gpu + Slot * sizeof(FDeferredMaterialPC), Classify.Skip(Layout.MaterialArgsOffset + Slot * (uint32)sizeof(RHI::FDispatchIndirectArguments)));
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
    }

    // Background, terrain and undeferred materials were never classified, so they keep the env pass.
    void FDefaultSceneRenderer::DeferredLightingPass(RHI::FCmdListH CL)
    {
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        static const FShaderH LightingCS = FShaderLibrary::Get("DeferredLighting.slang");
        if (!LightingCS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Deferred Lighting Pass", tracy::Color::Gold);
        SCENE_GPU_SCOPE(CL, "Deferred Lighting");

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            LOG_ERROR("Deferred lighting: the HDR target has no storage heap slot; skipping the pass.");
            return;
        }

        const RHI::FGPUAllocation Classify = GetMaterialClassify();

        struct FDeferredLightingPC
        {
            uint32      GBufferAIndex;
            uint32      GBufferBIndex;
            uint32      GBufferCIndex;
            uint32      GBufferDIndex;
            uint32      DepthIndex;
            uint32      HDRUAV;
            uint32      ScreenW;
            uint32      ScreenH;
            RHI::TGPUSpan<uint32> PixelList;
            RHI::TGPUSpan<uint32> Total;
        } PC = {};
        static_assert(sizeof(FDeferredLightingPC) == 64, "FDeferredLightingPC must match DeferredLighting.slang FDeferredLightingArgs.");
        PC.GBufferAIndex = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex    = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.HDRUAV        = (uint32)HDRUAV;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;
        PC.PixelList = { GetMaterialPixelList(), Layout.PixelCapacity };
        PC.Total     = RHI::TGPUSpan<uint32>::FromAddress(Classify.Gpu + Layout.TotalOffset, 1u);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LightingCS));
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), Classify.Skip(Layout.LightArgsOffset));

        // The lit HDR target is drawn into by the forward passes, sampled, and read by the post chain.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
                        RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }
    
    void FDefaultSceneRenderer::GTAOPass(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !IsGTAOEnabled())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GTAO Pass", tracy::Color::Red);

        static const FShaderH PrefilterCS = FShaderLibrary::Get("GTAOPrefilterDepth.slang");
        static const FShaderH MainCS      = FShaderLibrary::Get("GTAOMain.slang");
        static const FShaderH DenoiseCS   = FShaderLibrary::Get("GTAODenoise.slang");
        if (!PrefilterCS || !MainCS || !DenoiseCS)
        {
            return;
        }

        const FSceneImage& Depth        = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);
        const FSceneImage& Edges        = GetNamedImage(ENamedImage::GTAOEdges);
        const FSceneImage& TermA        = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& TermB        = GetNamedImage(ENamedImage::GTAODenoise);
        const FSceneImage& Output       = GetNamedImage(ENamedImage::GTAOBlur);

        const int32 DepthSlot = Depth.GetResourceID();
        if (DepthSlot < 0 || !WorkingDepth.IsValid() || WorkingDepth.GetNumMips() < GTAODepthMipLevels)
        {
            LOG_ERROR("GTAO working depth pyramid is missing or too shallow; skipping the pass.");
            return;
        }

        const uint32 Width  = Output.GetSizeX();
        const uint32 Height = Output.GetSizeY();

        float Radius                   = 0.5f;
        float Intensity                = 1.0f;
        float FinalValuePower          = 2.2f;
        float RadiusMultiplier         = 1.457f;
        float FalloffRange             = 0.615f;
        float SampleDistributionPower  = 2.0f;
        float ThinOccluderCompensation = 0.0f;
        float DepthMipSamplingOffset   = 3.3f;
        int32 QualityLevel             = 3;
        int32 DenoisePasses            = 1;

        if (const CRendererSettings* Settings = GetDefault<CRendererSettings>())
        {
            Radius                   = Settings->GTAORadius;
            Intensity                = Settings->GTAOIntensity;
            FinalValuePower          = Settings->GTAOPower;
            RadiusMultiplier         = Settings->GTAORadiusMultiplier;
            FalloffRange             = Settings->GTAOFalloffRange;
            SampleDistributionPower  = Settings->GTAOSampleDistributionPower;
            ThinOccluderCompensation = Settings->GTAOThinOccluderCompensation;
            DepthMipSamplingOffset   = Settings->GTAODepthMipSamplingOffset;
            QualityLevel             = Math::Clamp(Settings->GTAOQualityLevel, 0, 3);
            DenoisePasses            = Math::Clamp(Settings->GTAODenoisePasses, 0, 3);
        }

        // Every heuristic in the reference is tuned against this pre-multiplied radius, never the raw one.
        const float EffectRadius = Radius * RadiusMultiplier;

        {
            SCENE_GPU_SCOPE(CL, "GTAO Prefilter Depth");

            struct FPrefilterConstants
            {
                uint32 ViewportSize[2];
                uint32 SrcDepthIndex;
                uint32 MipUAV[GTAODepthMipLevels];
                float  EffectRadius;
                float  EffectFalloffRange;
            } PC = {};

            PC.ViewportSize[0]    = Width;
            PC.ViewportSize[1]    = Height;
            PC.SrcDepthIndex      = (uint32)DepthSlot;
            PC.EffectRadius       = EffectRadius;
            PC.EffectFalloffRange = FalloffRange;

            for (uint32 Mip = 0; Mip < GTAODepthMipLevels; ++Mip)
            {
                const int32 Slot = WorkingDepth.GetMipUAVIndex(Mip);
                if (Slot < 0)
                {
                    LOG_ERROR("GTAO working depth mip {} has no storage heap slot; skipping the pass.", Mip);
                    return;
                }
                PC.MipUAV[Mip] = (uint32)Slot;
            }

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(PrefilterCS));

            // One group covers a 16x16 tile, which reduces to exactly one texel of the last mip.
            constexpr uint32 PrefilterTile = 16u;
            RHI::CmdDispatch(CL, MakeArgs(PC),
                RenderUtils::GetGroupCount(Width, PrefilterTile),
                RenderUtils::GetGroupCount(Height, PrefilterTile), 1);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
        }

        {
            SCENE_GPU_SCOPE(CL, "GTAO Main");

            struct FMainConstants
            {
                uint32 ViewportSize[2];
                uint32 WorkingDepthIndex;
                uint32 AOTermUAV;
                uint32 EdgesUAV;
                uint32 SliceCount;
                uint32 StepsPerSlice;
                uint32 NoiseIndex;
                float  EffectRadius;
                float  EffectFalloffRange;
                float  SampleDistributionPower;
                float  ThinOccluderCompensation;
                float  FinalValuePower;
                float  DepthMIPSamplingOffset;
            } PC = {};

            const int32 WorkingDepthSlot = WorkingDepth.GetResourceID();
            const int32 TermSlot         = TermA.GetMipUAVIndex(0);
            const int32 EdgesSlot        = Edges.GetMipUAVIndex(0);
            if (WorkingDepthSlot < 0 || TermSlot < 0 || EdgesSlot < 0)
            {
                LOG_ERROR("GTAO main pass is missing a heap slot; skipping the pass.");
                return;
            }

            // Rotating the noise only pays off once something downstream accumulates across frames.
            const uint32 FrameIndex = (CurrentView != nullptr) ? CurrentView->TemporalFrameIndex : 0u;

            PC.ViewportSize[0]          = Width;
            PC.ViewportSize[1]          = Height;
            PC.WorkingDepthIndex        = (uint32)WorkingDepthSlot;
            PC.AOTermUAV                = (uint32)TermSlot;
            PC.EdgesUAV                 = (uint32)EdgesSlot;
            PC.SliceCount               = GGTAOSliceCounts[QualityLevel];
            PC.StepsPerSlice            = GGTAOStepsPerSlice[QualityLevel];
            PC.NoiseIndex               = (DenoisePasses > 0) ? (FrameIndex % 64u) : 0u;
            PC.EffectRadius             = EffectRadius;
            PC.EffectFalloffRange       = FalloffRange;
            PC.SampleDistributionPower  = SampleDistributionPower;
            PC.ThinOccluderCompensation = ThinOccluderCompensation;
            PC.FinalValuePower          = FinalValuePower;
            PC.DepthMIPSamplingOffset   = DepthMipSamplingOffset;

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(MainCS));
            RHI::CmdDispatch(CL, MakeArgs(PC),
                RenderUtils::GetGroupCount(Width, 8u),
                RenderUtils::GetGroupCount(Height, 8u), 1);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
        }

        {
            SCENE_GPU_SCOPE(CL, "GTAO Denoise");

            struct FDenoiseConstants
            {
                uint32 ViewportSize[2];
                uint32 AOTermIndex;
                uint32 EdgesIndex;
                uint32 OutputUAV;
                uint32 FinalApply;
                float  DenoiseBlurBeta;
                float  Intensity;
            } PC = {};

            const int32 EdgesSlot = Edges.GetResourceID();
            if (EdgesSlot < 0)
            {
                LOG_ERROR("GTAO edge texture has no sampled heap slot; skipping the denoise.");
                return;
            }

            PC.ViewportSize[0] = Width;
            PC.ViewportSize[1] = Height;
            PC.EdgesIndex      = (uint32)EdgesSlot;
            PC.DenoiseBlurBeta = (DenoisePasses == 0) ? GGTAODenoiseDisabledBeta : GGTAODenoiseBeta;
            PC.Intensity       = Intensity;

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DenoiseCS));

            // A disabled denoise still runs one pass, because the final apply is what undoes the packing scale.
            const uint32 NumPasses = (uint32)Math::Max(DenoisePasses, 1);
            for (uint32 PassIndex = 0; PassIndex < NumPasses; ++PassIndex)
            {
                const bool bFinal = (PassIndex + 1u) == NumPasses;
                const bool bEven  = (PassIndex & 1u) == 0u;

                const FSceneImage& Src = bEven ? TermA : TermB;
                const FSceneImage& Dst = bFinal ? Output : (bEven ? TermB : TermA);

                const int32 SrcSlot = Src.GetResourceID();
                const int32 DstSlot = Dst.GetMipUAVIndex(0);
                if (SrcSlot < 0 || DstSlot < 0)
                {
                    LOG_ERROR("GTAO denoise pass {} is missing a heap slot; stopping the chain.", PassIndex);
                    return;
                }

                PC.AOTermIndex = (uint32)SrcSlot;
                PC.OutputUAV   = (uint32)DstSlot;
                PC.FinalApply  = bFinal ? 1u : 0u;

                // Each thread denoises two horizontal pixels, so a group spans 16 across and 8 down.
                RHI::CmdDispatch(CL, MakeArgs(PC),
                    RenderUtils::GetGroupCount(Width, 16u),
                    RenderUtils::GetGroupCount(Height, 8u), 1);

                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
            }
        }
    }

    void FDefaultSceneRenderer::ShadowMaskPass(RHI::FCmdListH CL)
    {
        if (!FrameFlags.bShadowMaskValid)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Shadow Mask", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMaskPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = GetNamedImage(ENamedImage::ShadowMask);
        const FSceneImage& Depth  = GetNamedImage(ENamedImage::DepthAttachment);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;   // every pixel is written (sky included)
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 DepthIndex;
        } PC;

        PC.DepthIndex = (uint32)Depth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // MBOIT pass 1 must see the identical fragment set TransparentPass will shade.
    void FDefaultSceneRenderer::MomentGenerationPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Moment Generation Pass", tracy::Color::SteelBlue);

        const FSceneImage& MomentZeroth = GetNamedImage(ENamedImage::MomentZeroth);
        const FSceneImage& Moments      = GetNamedImage(ENamedImage::Moments);
        const FUIntVector2 Extent       = GetNamedImage(ENamedImage::HDR).GetExtent();

        // Zero absorbance reads as fully transmissive and composites as untouched background.
        RHI::FRenderAttachment Colors[2];
        Colors[0].Texture  = MomentZeroth.Texture;
        Colors[0].LoadOp   = RHI::ELoadOp::Clear;
        Colors[0].StoreOp  = RHI::EStoreOp::Store;
        Colors[0].Color[0] = Colors[0].Color[1] = Colors[0].Color[2] = Colors[0].Color[3] = 0.0f;
        Colors[1].Texture  = Moments.Texture;
        Colors[1].LoadOp   = RHI::ELoadOp::Clear;
        Colors[1].StoreOp  = RHI::EStoreOp::Store;
        Colors[1].Color[0] = Colors[1].Color[1] = Colors[1].Color[2] = Colors[1].Color[3] = 0.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, 2);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        // Depth-test but never write, so translucency neither hides behind walls nor self-occludes.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        RHI::FBlendDesc MomentBlend;
        MomentBlend.bBlendEnable   = true;
        MomentBlend.SrcColorFactor = RHI::EFactor::One;
        MomentBlend.DstColorFactor = RHI::EFactor::One;
        MomentBlend.SrcAlphaFactor = RHI::EFactor::One;
        MomentBlend.DstAlphaFactor = RHI::EFactor::One;

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (Batch.bAdditive || Batch.bModulate)
                {
                    return false;   // UnorderedTranslucentPass owns these; a commutative blend needs no moments
                }

                // A null shader means the material fell back to a default, so skip it out of the moments.
                if (Batch.MomentPixelShader == nullptr)
                {
                    return false;
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.MomentPixelShader;
                Key.DepthFormat = EFormat::D32;
                // TransparentPass must make the identical choice or the transmittance will not describe it.
                Key.TriCullMode = (uint8)(Batch.bTwoSided ? 0u : (uint32)TriCull_Backface);
                Key.ColorTargets.push_back({ MomentZeroth.Desc.Format, MomentBlend });
                Key.ColorTargets.push_back({ Moments.Desc.Format, MomentBlend });
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                // Honor the two-sided flag exactly as VisBufferPass does; these passes are ROP-bound.
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // MBOIT pass 2 weights each fragment by the transmittance in front of it; no revealage target.
    void FDefaultSceneRenderer::TransparentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Transparent Pass", tracy::Color::CadetBlue);

        const FSceneImage& Accum     = GetNamedImage(ENamedImage::Accum);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture  = Accum.Texture;
        Colors[0].LoadOp   = RHI::ELoadOp::Clear;
        Colors[0].StoreOp  = RHI::EStoreOp::Store;
        Colors[0].Color[0] = Colors[0].Color[1] = Colors[0].Color[2] = Colors[0].Color[3] = 0.0f;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture  = Picker.Texture;
        Colors[1].LoadOp   = RHI::ELoadOp::Load;
        Colors[1].StoreOp  = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        RHI::FBlendDesc AccumBlend;
        AccumBlend.bBlendEnable   = true;
        AccumBlend.SrcColorFactor = RHI::EFactor::One;
        AccumBlend.DstColorFactor = RHI::EFactor::One;
        AccumBlend.SrcAlphaFactor = RHI::EFactor::One;
        AccumBlend.DstAlphaFactor = RHI::EFactor::One;

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (Batch.bAdditive || Batch.bModulate)
                {
                    return false;   // UnorderedTranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                // MUST stay byte-identical to MomentGenerationPass' choice; see the comment there.
                Key.TriCullMode = (uint8)(Batch.bTwoSided ? 0u : (uint32)TriCull_Backface);
                Key.ColorTargets.push_back({ Accum.Desc.Format, AccumBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::OITResolvePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("OIT Resolve Pass", tracy::Color::GreenYellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("OITResolve.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR          = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Accum        = GetNamedImage(ENamedImage::Accum);
        const FSceneImage& MomentZeroth = GetNamedImage(ENamedImage::MomentZeroth);
        const FSceneImage& Moments      = GetNamedImage(ENamedImage::Moments);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Added, not lerped, since each fragment was already pre-weighted by its transmittance.
        RHI::FBlendDesc CompositeBlend;
        CompositeBlend.bBlendEnable   = true;
        CompositeBlend.SrcColorFactor = RHI::EFactor::One;
        CompositeBlend.DstColorFactor = RHI::EFactor::SrcAlpha;
        CompositeBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        CompositeBlend.DstAlphaFactor = RHI::EFactor::One;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ HDR.Desc.Format, CompositeBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FOITResolvePushConstants
        {
            uint32 AccumIndex;
            uint32 MomentZerothIndex;
            uint32 MomentsIndex;
            uint32 _Pad0;
        };
        static_assert(sizeof(FOITResolvePushConstants) == 16, "FOITResolvePushConstants must match the slang pass block.");

        FOITResolvePushConstants PC = {};
        PC.AccumIndex        = (uint32)Accum.GetResourceID();
        PC.MomentZerothIndex = (uint32)MomentZeroth.GetResourceID();
        PC.MomentsIndex      = (uint32)Moments.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::UnorderedTranslucentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands        = Frame.Geometry.DrawCommands;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        bool bHasUnordered = false;
        for (uint32 Idx : TranslucentDrawList)
        {
            if (DrawCommands[Idx].bAdditive || DrawCommands[Idx].bModulate)
            {
                bHasUnordered = true;
                break;
            }
        }
        if (!bHasUnordered)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Unordered Translucent Pass", tracy::Color::CadetBlue3);

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const FUIntVector2 Extent = HDR.GetExtent();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = RHI::ELoadOp::Load;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AdditiveBlend;
        AdditiveBlend.bBlendEnable   = true;
        AdditiveBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AdditiveBlend.DstColorFactor = RHI::EFactor::One;
        AdditiveBlend.SrcAlphaFactor = RHI::EFactor::One;
        AdditiveBlend.DstAlphaFactor = RHI::EFactor::One;

        // Src is the shader's lerp toward white, so this is scene color times the faded material color.
        RHI::FBlendDesc ModulateBlend;
        ModulateBlend.bBlendEnable   = true;
        ModulateBlend.SrcColorFactor = RHI::EFactor::DstColor;
        ModulateBlend.DstColorFactor = RHI::EFactor::Zero;
        ModulateBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        ModulateBlend.DstAlphaFactor = RHI::EFactor::One;

        // Resolved for the same reason as the WBOIT pass above, after the mid pyramid rebuild.
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (!Batch.bAdditive && !Batch.bModulate)
                {
                    return false;   // TranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                Key.ColorTargets.push_back({ HDR.Desc.Format, Batch.bModulate ? ModulateBlend : AdditiveBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                // Per batch, since a depth-writing blend is a material choice and the pass mixes both.
                RHI::FDepthStencilDesc DepthDesc;
                DepthDesc.DepthMode = Batch.bWriteDepth
                                    ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                                    : RHI::EDepthFlags::Read;
                DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
                RHI::CmdSetDepthStencil(CL, (DepthDesc));
            });

        // Depth write is dynamic state, so a writing batch would otherwise hand it to the next pass.
        RHI::FDepthStencilDesc RestoreDepth;
        RestoreDepth.DepthMode = RHI::EDepthFlags::Read;
        RestoreDepth.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (RestoreDepth));

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        // Mirrors the FPushConstants in the three VolumetricFog*.slang shaders.
        struct FFroxelInjectPushConstants
        {
            uint32   GridSize[3];
            float    NearPlane;

            float    FogRange;            // froxel far plane (max fog distance, view units)
            uint32   bSunVolumetric;      // 1 if light 0 (sun) opted into volumetrics
            float    Time;
            uint32   ScatterUAV;          // bindless 3D UAV index of the scatter volume

            uint32   bSupersampleLocal;   // 1 = 4x supersample local light in-scatter per froxel
            uint32   _PadNumFogVolumes;
            uint32   CloudShadowIndex;    // bindless 2D SRV, ~0u when no cloud shadow was built
            float    CloudShadowExtent;

            float    CloudShadowCenter[2];
            float    _Pad0[2];

            RHI::TGPUSpan<FGPUFogVolume> FogVolumes;   // offset 64, 8-aligned
        };
        static_assert(sizeof(FFroxelInjectPushConstants) <= 128, "Froxel inject PC must fit 128B");
        static_assert(offsetof(FFroxelInjectPushConstants, FogVolumes) % 8 == 0, "PC pointer must be 8-aligned");

        struct FCloudShadowPushConstants
        {
            // Mirrors FCloudMedium in Includes/CloudCommon.slang.
            uint32 NoiseIndex;
            float  ShapeScale;
            float  DetailScale;
            float  DetailStrength;

            float  Billow;
            float  Coverage;
            float  Density;
            float  LayerBottom;

            float  LayerTop;
            float  _MediumPad0;
            float  WindOffset[2];

            float  DetailWindOffset[2];
            float  _MediumPad1[2];

            uint32 ShadowUAV;
            uint32 Resolution;
            uint32 MarchSteps;
            float  HalfExtent;

            float  Center[2];
            float  Absorption;
            float  _Pad0;

            float  SunDirection[3];
            float  _Pad1;
        };
        static_assert(sizeof(FCloudShadowPushConstants) <= 128, "Cloud shadow PC must fit 128B");

        struct FFroxelIntegratePushConstants
        {
            uint32 GridSize[3];
            float  NearPlane;
            float  FogRange;
            uint32 ScatterSRV;     // bindless 3D SRV index of the scatter volume
            uint32 IntegratedUAV;  // bindless 3D UAV index of the integrated volume
            uint32 _Pad0;
        };

        struct FAtmosphereCompositePushConstants
        {
            uint32 HDRUAV;
            uint32 DepthIndex;       // bindless 2D SRV of scene depth
            uint32 ScreenW;
            uint32 ScreenH;

            uint32 AerialInScatterIndex;      // ~0u disables aerial perspective
            uint32 AerialTransmittanceIndex;
            float  AerialRange;
            float  AerialIntensity;

            uint32 CloudScatterIndex;         // ~0u disables clouds
            uint32 bFog;
            uint32 FroxelIntegratedIndex;     // bindless 3D SRV of the integrated froxel volume
            uint32 GridZ;

            float  NearPlane;
            float  FogRange;
            uint32 bVolumetric;      // froxel volume valid this frame; 0 = analytic height fog only
            uint32 FarShaftSteps;    // 0 = far field stays closed-form and unshadowed

            float  FarShaftDistance;
            uint32 CloudShadowIndex; // bindless 2D SRV, ~0u when no cloud shadow was built
            float  CloudShadowExtent;
            float  CloudShadowCenterX;

            float  CloudShadowCenterY;
            float  _Pad0;
            float  _Pad1;
            float  _Pad2;
        };
        static_assert(sizeof(FAtmosphereCompositePushConstants) == 96,
            "FAtmosphereCompositePushConstants must match AtmosphereComposite.slang::FPushConstants.");

        constexpr uint32 AtmosphereTileSize = 8;
    }

    void FDefaultSceneRenderer::PublishFogGlobals(FSceneGlobalData& Globals) const
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bHasFog      = Frame.Volumetrics.bHasFog;
        const bool bVolumetric  = bHasFog && Frame.Volumetrics.bVolumetricFog;

        Globals.FogParams           = Frame.Volumetrics.FogParams;
        Globals.bFogEnabled         = bHasFog ? 1u : 0u;
        Globals.FogGridZ            = FroxelGridSize.z;
        Globals.FogNearPlane        = Math::Max(Globals.NearPlane, 0.05f);
        Globals.FogRange            = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Globals.FarPlane);
        Globals.FogFarShaftSteps    = Frame.Volumetrics.FarShaftSteps;
        Globals.FogFarShaftDistance = Frame.Volumetrics.FarShaftDistance;

        Globals.FogIntegratedIndex = bVolumetric
            ? (uint32)CurrentView->Images[(int)ENamedImage::FroxelIntegrated].GetResourceID()
            : ~0u;

        Globals.FogCloudShadowIndex  = ~0u;
        Globals.FogCloudShadowExtent = 0.0f;
        Globals.FogCloudShadowCenter = FVector2(0.0f, 0.0f);

        bool  bCloudShadows = true;
        float Extent        = 4000.0f;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            bCloudShadows = RS->bCloudShadows;
            Extent        = Math::Max(RS->CloudShadowExtent, 100.0f);
        }
        if (!bCloudShadows || !bHasFog || !Frame.Volumetrics.bClouds)
        {
            return;
        }

        const FSceneImage& Shadow = CurrentView->Images[(int)ENamedImage::CloudShadow];
        if (!Shadow.IsValid())
        {
            return;
        }

        // Snapped to its own texel grid, or every camera step reshuffles the integral and the fog crawls.
        const float Texel = (2.0f * Extent) / (float)Math::Max(Shadow.GetSizeX(), 1u);
        const FVector3 Cam = FVector3(Globals.CameraData.Location);

        Globals.FogCloudShadowIndex  = (uint32)Shadow.GetResourceID();
        Globals.FogCloudShadowExtent = Extent;
        Globals.FogCloudShadowCenter = FVector2(Math::Floor(Cam.x / Texel) * Texel,
                                                Math::Floor(Cam.z / Texel) * Texel);
    }

    void FDefaultSceneRenderer::CloudShadowMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.SceneGlobalData.FogCloudShadowIndex == ~0u)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("CloudShadowMap.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Noise  = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Shadow = GetNamedImage(ENamedImage::CloudShadow);
        if (!Noise.IsValid() || !Shadow.IsValid() || !CurrentView->bCloudNoiseBaked)
        {
            return;
        }

        const int32 ShadowUAV = Shadow.GetMipUAVIndex(0);
        if (ShadowUAV < 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cloud Shadow Map", tracy::Color::LightSlateGray);

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        const FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector2 Wind       = C.WindDirection;
        const float WindLen = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);
        const float Drift = C.WindSpeed * Frame.SceneGlobalData.Time;

        int32 Steps = 8;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            Steps = Math::Clamp(RS->CloudShadowSteps, 1, 32);
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        FCloudShadowPushConstants PC = {};
        PC.NoiseIndex          = (uint32)Noise.GetResourceID();
        PC.ShapeScale          = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale         = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength      = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow              = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.Coverage            = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density             = Math::Max(C.Density, 0.0f);
        PC.LayerBottom         = Math::Max(C.LayerBottom, 100.0f);
        PC.LayerTop            = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        PC.WindOffset[0]       = Wind.x * Drift;
        PC.WindOffset[1]       = Wind.y * Drift;
        PC.DetailWindOffset[0] = Wind.x * Drift * 2.0f;
        PC.DetailWindOffset[1] = Wind.y * Drift * 2.0f;

        PC.ShadowUAV       = (uint32)ShadowUAV;
        PC.Resolution      = Shadow.GetSizeX();
        PC.MarchSteps      = (uint32)Steps;
        PC.HalfExtent      = Frame.SceneGlobalData.FogCloudShadowExtent;
        PC.Center[0]       = Frame.SceneGlobalData.FogCloudShadowCenter.x;
        PC.Center[1]       = Frame.SceneGlobalData.FogCloudShadowCenter.y;
        PC.Absorption      = 1.0f;
        PC.SunDirection[0] = SunDir.x;
        PC.SunDirection[1] = SunDir.y;
        PC.SunDirection[2] = SunDir.z;

        const uint32 Groups = RenderUtils::GetGroupCount(Shadow.GetSizeX(), 8);
        RHI::CmdDispatch(CL, MakeArgs(PC), Groups, Groups, 1u);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::FroxelInjectPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        const auto& LightData       = Frame.Lighting.LightData;
        const auto& SceneGlobalData = Frame.SceneGlobalData;

        const bool bSunVolumetric = LightData.Lights.Count > 0
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Directional)
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Volumetric);

        LUMINA_PROFILE_SECTION_COLORED("Froxel Inject Pass", tracy::Color::SlateBlue);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogInject.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter = GetNamedImage(ENamedImage::FroxelScatter);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const float FogRange = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, SceneGlobalData.FarPlane);
        const uint32 NumVolumes = Math::Min((uint32)Frame.Volumetrics.FogVolumes.size(), GFogMaxVolumes);

        FFroxelInjectPushConstants PC = {};
        PC.ScatterUAV           = (uint32)Scatter.GetMipUAVIndex(0);
        PC.GridSize[0]          = FroxelGridSize.x;
        PC.GridSize[1]          = FroxelGridSize.y;
        PC.GridSize[2]          = FroxelGridSize.z;
        PC.NearPlane            = Math::Max(SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange             = FogRange;
        PC.bSunVolumetric       = bSunVolumetric ? 1u : 0u;
        PC.Time                 = SceneGlobalData.Time;

        PC.CloudShadowIndex     = SceneGlobalData.FogCloudShadowIndex;
        PC.CloudShadowExtent    = SceneGlobalData.FogCloudShadowExtent;
        PC.CloudShadowCenter[0] = SceneGlobalData.FogCloudShadowCenter.x;
        PC.CloudShadowCenter[1] = SceneGlobalData.FogCloudShadowCenter.y;
        PC.bSupersampleLocal    = 1u;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            PC.bSupersampleLocal = RS->bSupersampleVolumetricLights ? 1u : 0u;
        }
        if (NumVolumes > 0)
        {
            PC.FogVolumes = RHI::CopyTransientArray(Frame.Volumetrics.FogVolumes.data(), NumVolumes);
        }

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.z, 4));

        // Integrate reads the scatter volume next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::FroxelIntegratePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Integrate Pass", tracy::Color::MediumPurple);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogIntegrate.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter    = GetNamedImage(ENamedImage::FroxelScatter);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const float FogRange = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);

        FFroxelIntegratePushConstants PC = {};
        PC.GridSize[0]    = FroxelGridSize.x;
        PC.GridSize[1]    = FroxelGridSize.y;
        PC.GridSize[2]    = FroxelGridSize.z;
        PC.NearPlane      = Math::Max(Frame.SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange       = FogRange;
        PC.ScatterSRV     = (uint32)Scatter.GetResourceID();
        PC.IntegratedUAV  = (uint32)Integrated.GetMipUAVIndex(0);

        // One thread per (x,y) column; each marches the full Z range.
        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 8),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 8),
                         1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::AtmosphereCompositePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        const bool bFog    = Frame.Volumetrics.bHasFog;
        const bool bAerial = AtmosphereTerms.AerialInScatterIndex != ~0u;
        const bool bClouds = AtmosphereTerms.CloudScatterIndex != ~0u;

        if (!bFog && !bAerial && !bClouds)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("AtmosphereComposite.slang");
        if (!CS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Atmosphere Composite Pass", tracy::Color::Orange3);

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        const uint32 Width  = HDR.GetSizeX();
        const uint32 Height = HDR.GetSizeY();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        FAtmosphereCompositePushConstants PC = {};
        PC.HDRUAV     = (uint32)HDRUAV;
        PC.DepthIndex = (uint32)SceneDepth.GetResourceID();
        PC.ScreenW    = Width;
        PC.ScreenH    = Height;

        PC.AerialInScatterIndex     = AtmosphereTerms.AerialInScatterIndex;
        PC.AerialTransmittanceIndex = AtmosphereTerms.AerialTransmittanceIndex;
        PC.AerialRange              = AtmosphereTerms.AerialRange;
        PC.AerialIntensity          = AtmosphereTerms.AerialIntensity;

        PC.CloudScatterIndex     = AtmosphereTerms.CloudScatterIndex;
        PC.bFog                  = bFog ? 1u : 0u;
        PC.FroxelIntegratedIndex = (uint32)Integrated.GetResourceID();
        PC.GridZ                 = FroxelGridSize.z;

        PC.NearPlane          = Math::Max(Frame.SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange           = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);
        PC.bVolumetric        = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;
        PC.FarShaftSteps      = Frame.Volumetrics.FarShaftSteps;
        PC.FarShaftDistance   = Frame.Volumetrics.FarShaftDistance;
        PC.CloudShadowIndex   = Frame.SceneGlobalData.FogCloudShadowIndex;
        PC.CloudShadowExtent  = Frame.SceneGlobalData.FogCloudShadowExtent;
        PC.CloudShadowCenterX = Frame.SceneGlobalData.FogCloudShadowCenter.x;
        PC.CloudShadowCenterY = Frame.SceneGlobalData.FogCloudShadowCenter.y;

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(Width,  AtmosphereTileSize),
                         RenderUtils::GetGroupCount(Height, AtmosphereTileSize), 1);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::WaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUWater>& Waters = Frame.Water.Surfaces;
        if (Waters.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Water Pass", tracy::Color::CadetBlue);

        static const FShaderH VS = FShaderLibrary::Get("WaterVert.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterPixel.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        Barriers::SceneToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToShaders(CL);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        // Double-sided so the surface is visible from below (camera submerged) too.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Standard alpha over, where the PS composites scene and water and alpha softens the shore.
        RHI::FBlendDesc WaterBlend;
        WaterBlend.bBlendEnable   = true;
        WaterBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        WaterBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        WaterBlend.SrcAlphaFactor = RHI::EFactor::One;
        WaterBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VS;
        Key.PS = PS;
        Key.ColorTargets.push_back({ HDR.Desc.Format, WaterBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FWaterPushConstants
        {
            RHI::TGPUSpan<FGPUWater> Waters;
            uint32 SceneColorIndex;
            uint32 SceneDepthIndex;
        };
        static_assert(sizeof(FWaterPushConstants) == 24, "FWaterPushConstants must match Includes/Water.slang.");

        FWaterPushConstants PC = {};
        PC.Waters          = RHI::CopyTransientArray(Waters.data(), Waters.size());
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.SceneDepthIndex = (uint32)SceneDepth.GetResourceID();

        const RHI::GPUPtr Args = MakeArgs(PC);

        for (uint32 i = 0; i < (uint32)Waters.size(); ++i)
        {
            const uint32 Res = Waters[i].GridResolution;
            const uint32 VertexCount = (Res > 1u) ? (Res - 1u) * (Res - 1u) * 6u : 0u;
            if (VertexCount == 0u)
            {
                continue;
            }

            RHI::CmdDraw(CL, Args, VertexCount, 1, 0, i);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::UnderwaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Water.bUnderwaterActive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Underwater Pass", tracy::Color::SteelBlue);

        static const FShaderH VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterUnderwater.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        Barriers::SceneToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToShaders(CL);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VS;
        Key.PS = PS;
        Key.ColorTargets.push_back({ HDR.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FUnderwaterPushConstants
        {
            uint64 ParamsAddr;
            uint32 SceneColorIndex;
            uint32 SceneDepthIndex;
        };
        static_assert(sizeof(FUnderwaterPushConstants) == 16, "FUnderwaterPushConstants must match WaterUnderwater.slang.");

        FUnderwaterPushConstants PC = {};
        PC.ParamsAddr      = RHI::CopyTransient(Frame.Water.Underwater);
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.SceneDepthIndex = (uint32)SceneDepth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
    
    void FDefaultSceneRenderer::SkyCubeCapturePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Cube Capture", tracy::Color::SkyBlue);

        const FFrameData& Frame = *RenderFrame;
        const auto& LightData         = Frame.Lighting.LightData;
        const auto& SceneGlobalData   = Frame.SceneGlobalData;
        const int32 EnvironmentMapID  = Frame.Volumetrics.EnvironmentMapID;
        const bool bIBLDirty          = Frame.Volumetrics.bIBLDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EStageFlags::Transfer);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyCube).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyIrradiance).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyPrefilter).Texture, Black);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
            return;
        }

        if (!bIBLDirty)
        {
            return;
        }

        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        if (!SkyCube.IsValid())
        {
            return;
        }

        // HDRI path where equirect to cube replaces the procedural fill.
        if (EnvironmentMapID >= 0)
        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("EquirectToCubemap.slang");
            if (!ComputeShader)
            {
                return;
            }

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

            struct FEquirectPC
            {
                uint32 EquirectSRV;
                uint32 SkyCubeUAV;
                float  Intensity;
                float  CosYaw;
                float  SinYaw;
                uint32 _Pad0;
            };
            static_assert(sizeof(FEquirectPC) == 24, "FEquirectPC must match EquirectToCubemap.slang::FPushConstants.");

            const FVector4& HDRIParams = Frame.Volumetrics.EnvironmentParams.HDRIParams;

            FEquirectPC PC = {};
            PC.EquirectSRV = (uint32)EnvironmentMapID;
            PC.SkyCubeUAV  = (uint32)SkyCube.GetMipUAVIndex(0);
            PC.Intensity   = HDRIParams.x;
            PC.CosYaw      = HDRIParams.y;
            PC.SinYaw      = HDRIParams.z;

            constexpr uint32 EquirectTile = 8u;
            const uint32 FaceSize = SkyCube.GetSizeX();
            const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, EquirectTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("SkyCubeCapture.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FSkyCapturePC
        {
            uint64   EnvAddr;
            uint32   SkyCubeUAV;
            float    Time;
            FVector3 SunDirection;
            float    _Pad;
        } PC = {};
        PC.EnvAddr    = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        PC.SkyCubeUAV = (uint32)SkyCube.GetMipUAVIndex(0);

        if (LightData.bHasSun)
        {
            PC.SunDirection = Math::Normalize(LightData.SunDirection);
        }
        else
        {
            PC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }
        PC.Time = SceneGlobalData.Time;

        constexpr uint32 SkyCaptureTile = 8u;
        const uint32 FaceSize = SkyCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, SkyCaptureTile);
        // Z is 6 layers, one per cube face, and each thread owns one (face, x, y).
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        // Convolution + environment pass read the cube next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
    }

    void FDefaultSceneRenderer::IrradianceConvolutionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Irradiance Convolution", tracy::Color::SkyBlue1);

        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        const FSceneImage& SkyCube        = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& IrradianceCube = GetNamedImage(ENamedImage::SkyIrradiance);
        if (!SkyCube.IsValid() || !IrradianceCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("IrradianceConvolution.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FIrradiancePC { uint32 SrcCubeSRV; uint32 OutCubeUAV; uint32 _Pad0; uint32 _Pad1; };
        FIrradiancePC PC = {};
        PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
        PC.OutCubeUAV = (uint32)IrradianceCube.GetMipUAVIndex(0);

        constexpr uint32 IrradianceTile = 8u;
        const uint32 FaceSize = IrradianceCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, IrradianceTile);
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::PrefilterEnvMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Sky Prefilter Convolution", tracy::Color::SkyBlue2);

        const FSceneImage& SkyCube       = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& PrefilterCube = GetNamedImage(ENamedImage::SkyPrefilter);
        if (!SkyCube.IsValid() || !PrefilterCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        const uint32 NumMips      = PrefilterCube.GetNumMips();
        const uint32 BaseFaceSize = PrefilterCube.GetSizeX();

        constexpr uint32 PrefilterTile = 8u;

        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            FPrefilterPC PC = {};
            PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
            PC.OutMipUAV  = (uint32)PrefilterCube.GetMipUAVIndex(Mip);
            PC.Roughness  = (NumMips <= 1u) ? 0.0f
                                            : (float)Mip / (float)(NumMips - 1u);
            PC.NumSamples = GPrefilterSampleCount;

            const uint32 MipFaceSize = std::max<uint32>(BaseFaceSize >> Mip, 1u);
            const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::EnvironmentPass(RHI::FCmdListH CL)
    {
        if (!FrameFlags.bHasEnvironment)
        {
            const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);

            RHI::FRenderAttachment Color;
            Color.Texture        = ColorRT.Texture;
            Color.LoadOp         = RHI::ELoadOp::Clear;
            Color.StoreOp        = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Environment Pass", tracy::Color::Green3);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("Environment.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        const int32  EnvMapID    = RenderFrame->Volumetrics.EnvironmentMapID;
        const uint32 EquirectIdx = EnvMapID >= 0 ? (uint32)EnvMapID : (uint32)GetNamedImage(ENamedImage::BRDFLut).GetResourceID();
        const uint32 EquirectW   = EnvMapID >= 0 ? RenderFrame->Volumetrics.EnvironmentMapWidth : 256u;

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorRT.Texture;
        Color.LoadOp         = RHI::ELoadOp::Clear;
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Mirrors the bit-cast the extract does into Misc.x; specializing on it strips the other modes.
        uint32 SkyModeBits = GSkyMode_Runtime;
        std::memcpy(&SkyModeBits, &RenderFrame->Volumetrics.EnvironmentParams.Misc.x, sizeof(uint32));

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.SkyMode     = (SkyModeBits <= GSkyMode_HDRI) ? (uint8)SkyModeBits : (uint8)GSkyMode_Runtime;
        Key.ColorTargets.push_back({ ColorRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FEnvPushConstants
        {
            uint64 EnvAddr;
            uint32 SkyCubeIndex;
            uint32 EquirectIndex;
            uint32 EquirectWidth;   // HDRI mode LOD that anti-aliases the sky
            uint32 _Pad1;
        };
        static_assert(sizeof(FEnvPushConstants) == 24, "FEnvPushConstants must match the slang pass block.");

        FEnvPushConstants PC = {};
        PC.EnvAddr       = RHI::CopyTransient(RenderFrame->Volumetrics.EnvironmentParams);
        PC.SkyCubeIndex  = (uint32)SkyCube.GetResourceID();
        PC.EquirectIndex = EquirectIdx;
        PC.EquirectWidth = EquirectW;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FAerialLUTPushConstants
        {
            uint64   EnvAddr;
            uint32   InScatterUAV;
            uint32   TransmittanceUAV;

            FVector3 SunDirection;
            float    Range;
        };
        static_assert(sizeof(FAerialLUTPushConstants) == 32,
            "FAerialLUTPushConstants must match AerialPerspectiveLUT.slang::FPushConstants.");

        constexpr uint32 AerialTileSize = 8;
    }

    namespace
    {
        struct FCloudNoisePushConstants
        {
            uint32 NoiseUAV;
            uint32 _Pad0;
            uint32 _Pad1;
            uint32 _Pad2;
        };
        static_assert(sizeof(FCloudNoisePushConstants) == 16,
            "FCloudNoisePushConstants must match CloudNoiseBake.slang::FPushConstants.");

        struct FCloudPushConstants
        {
            uint32   ScatterUAV;
            uint32   DepthIndex;
            uint32   NoiseIndex;
            uint32   ScreenW;

            uint32   ScreenH;
            uint32   MarchSteps;
            uint32   LightSteps;
            float    MaxDistance;

            FVector3 SunDirection;
            float    LayerBottom;

            FVector3 SunColor;
            float    LayerTop;

            FVector3 AmbientColor;
            float    Coverage;

            float    Density;
            float    ShapeScale;
            float    DetailScale;
            float    DetailStrength;

            float    Billow;
            float    ForwardScattering;
            float    BackScattering;
            float    PowderStrength;

            FVector2 WindOffset;
            FVector2 DetailWindOffset;
        };
        static_assert(sizeof(FCloudPushConstants) == 128,
            "FCloudPushConstants must match VolumetricClouds.slang::FPushConstants.");

        constexpr uint32 CloudTileSize      = 8;
        constexpr uint32 CloudNoiseTileSize = 4;
    }

    void FDefaultSceneRenderer::VolumetricCloudPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.CloudScatterIndex = ~0u;

        if (!Frame.Volumetrics.bClouds)
        {
            return;
        }

        static const FShaderH BakeCS  = FShaderLibrary::Get("CloudNoiseBake.slang");
        static const FShaderH CloudCS = FShaderLibrary::Get("VolumetricClouds.slang");
        if (!BakeCS || !CloudCS)
        {
            return;
        }

        const FSceneImage& Noise   = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Scatter = GetNamedImage(ENamedImage::CloudScatter);
        const FSceneImage& Depth   = GetNamedImage(ENamedImage::DepthAttachment);
        if (!Noise.IsValid() || !Scatter.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Volumetric Clouds", tracy::Color::White);

        // The volume is view-independent and static, so it is generated once and kept.
        if (!CurrentView->bCloudNoiseBaked)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BakeCS));

            FCloudNoisePushConstants BakePC = {};
            BakePC.NoiseUAV = (uint32)Noise.GetMipUAVIndex(0);

            const uint32 BakeGroups = RenderUtils::GetGroupCount(kCloudNoiseSize, CloudNoiseTileSize);
            RHI::CmdDispatch(CL, MakeArgs(BakePC), BakeGroups, BakeGroups, BakeGroups);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
            CurrentView->bCloudNoiseBaked = true;
        }

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector3 SunColor = FVector3(1.0f);
        if (LightData.bHasSun && LightData.Lights.Count > 0)
        {
            const FLight&  Sun    = Frame.Lighting.Lights[0];
            const FVector4 Unpack = UnpackColor(Sun.Color);
            SunColor = FVector3(Unpack.x, Unpack.y, Unpack.z) * Sun.Intensity;
        }

        const float Time     = Frame.SceneGlobalData.Time;
        FVector2 Wind        = C.WindDirection;
        const float WindLen  = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);

        const float Drift = C.WindSpeed * Time;

        const int32 ScatterUAV = Scatter.GetMipUAVIndex(0);
        if (ScatterUAV < 0)
        {
            return;
        }

        FCloudPushConstants PC = {};
        PC.ScatterUAV        = (uint32)ScatterUAV;
        PC.DepthIndex        = (uint32)Depth.GetResourceID();
        PC.NoiseIndex        = (uint32)Noise.GetResourceID();
        PC.ScreenW           = Scatter.GetSizeX();
        PC.ScreenH           = Scatter.GetSizeY();
        PC.MarchSteps        = (uint32)Math::Clamp(C.MarchSteps, 16, 256);
        PC.LightSteps        = (uint32)Math::Clamp(C.LightSteps, 1, 16);
        PC.MaxDistance       = Math::Max(C.MaxDistance, 1000.0f);
        PC.SunDirection      = SunDir;
        PC.LayerBottom       = Math::Max(C.LayerBottom, 100.0f);
        PC.SunColor          = SunColor * Math::Max(C.SunIntensity, 0.0f);
        PC.LayerTop          = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        PC.AmbientColor      = FVector3(LightData.AmbientLight.x, LightData.AmbientLight.y, LightData.AmbientLight.z)
                             * LightData.AmbientLight.w * Math::Max(C.AmbientIntensity, 0.0f);
        PC.Coverage          = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density           = Math::Max(C.Density, 0.0f);
        PC.ShapeScale        = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale       = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength    = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow            = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.ForwardScattering = Math::Clamp(C.ForwardScattering, 0.0f, 0.95f);
        PC.BackScattering    = Math::Clamp(C.BackScattering, -0.95f, 0.0f);
        PC.PowderStrength    = Math::Clamp(C.PowderStrength, 0.0f, 1.0f);
        PC.WindOffset        = FVector2(Wind.x * Drift, Wind.y * Drift);
        PC.DetailWindOffset  = FVector2(PC.WindOffset.x * C.DetailWindFactor,
                                        PC.WindOffset.y * C.DetailWindFactor);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CloudCS));
        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(PC.ScreenW, CloudTileSize),
                         RenderUtils::GetGroupCount(PC.ScreenH, CloudTileSize), 1);

        // The scatter volume feeds AtmosphereCompositePass and the cloud shadow map, both dispatches.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

        AtmosphereTerms.CloudScatterIndex = (uint32)Scatter.GetResourceID();
    }

    void FDefaultSceneRenderer::ScreenSpaceReflectionsPass(RHI::FCmdListH CL)
    {
        const CRendererSettings* RS = GetDefault<CRendererSettings>();
        if (RS == nullptr || !RS->bScreenSpaceReflections || RS->SSRIntensity <= 0.0f)
        {
            return;
        }

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        static const FShaderH SSRCS = FShaderLibrary::Get("ScreenSpaceReflections.slang");
        if (!SSRCS)
        {
            return;
        }

        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        if (!SceneColor.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Screen Space Reflections", tracy::Color::Cyan3);

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);
        const RHI::FGPUAllocation Classify = GetMaterialClassify();

        // This walks the classify block's pixel list, whose offsets only exist once the pass has run.
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        // The trace reads neighboring pixels while the composite writes this one, so it needs a snapshot.
        Barriers::SceneToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToShaders(CL);

        struct FSSRPushConstants
        {
            uint32      GBufferAIndex;
            uint32      GBufferBIndex;
            uint32      GBufferCIndex;
            uint32      GBufferDIndex;

            uint32      DepthIndex;
            uint32      HDRUAV;
            uint32      SceneColorIndex;
            uint32      ScreenW;

            uint32      ScreenH;
            uint32      MaxSteps;
            float       MaxDistance;
            float       Thickness;

            float       Intensity;
            float       RoughnessFade;
            uint32      _Pad0;
            uint32      _Pad1;

            RHI::TGPUSpan<uint32> PixelList;
            RHI::TGPUSpan<uint32> Total;
        } PC = {};
        static_assert(sizeof(FSSRPushConstants) == 96, "FSSRPushConstants must match ScreenSpaceReflections.slang FSSRArgs.");

        PC.GBufferAIndex   = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex   = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex   = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex   = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex      = (uint32)Depth.GetResourceID();
        PC.HDRUAV          = (uint32)HDRUAV;
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.ScreenW         = HDR.GetSizeX();
        PC.ScreenH         = HDR.GetSizeY();
        PC.MaxSteps        = (uint32)Math::Clamp(RS->SSRMaxSteps, 4, 128);
        PC.MaxDistance     = Math::Max(RS->SSRMaxDistance, 1.0f);
        PC.Thickness       = Math::Max(RS->SSRThickness, 0.01f);
        PC.Intensity       = Math::Clamp(RS->SSRIntensity, 0.0f, 1.0f);
        PC.RoughnessFade   = Math::Clamp(RS->SSRRoughnessFade, 0.0f, 1.0f);
        PC.PixelList = { GetMaterialPixelList(), Layout.PixelCapacity };
        PC.Total     = RHI::TGPUSpan<uint32>::FromAddress(Classify.Gpu + Layout.TotalOffset, 1u);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SSRCS));
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), Classify.Skip(Layout.LightArgsOffset));

        // HDR is a UAV write here, then a color attachment, a sampled input, and a post-chain read.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
                        RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::AerialPerspectivePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.AerialInScatterIndex     = ~0u;
        AtmosphereTerms.AerialTransmittanceIndex = ~0u;

        if (!FrameFlags.bHasEnvironment
            || !Frame.Volumetrics.bAerialPerspective
            || Frame.Volumetrics.AerialIntensity <= 0.0f)
        {
            return;
        }

        static const FShaderH LutCS = FShaderLibrary::Get("AerialPerspectiveLUT.slang");
        if (!LutCS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Aerial Perspective Pass", tracy::Color::LightSkyBlue);

        const FSceneImage& InScatter     = GetNamedImage(ENamedImage::AerialInScatter);
        const FSceneImage& Transmittance = GetNamedImage(ENamedImage::AerialTransmittance);

        if (!InScatter.IsValid() || !Transmittance.IsValid())
        {
            return;
        }

        const float Range = Math::Max(Frame.Volumetrics.AerialRange, 100.0f);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LutCS));

        FAerialLUTPushConstants LutPC = {};
        LutPC.EnvAddr          = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        LutPC.InScatterUAV     = (uint32)InScatter.GetMipUAVIndex(0);
        LutPC.TransmittanceUAV = (uint32)Transmittance.GetMipUAVIndex(0);
        LutPC.Range            = Range;

        if (Frame.Lighting.LightData.bHasSun)
        {
            LutPC.SunDirection = Math::Normalize(Frame.Lighting.LightData.SunDirection);
        }
        else
        {
            LutPC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }

        const uint32 LutGroups = RenderUtils::GetGroupCount(kAerialLUTSize, AerialTileSize);
        RHI::CmdDispatch(CL, MakeArgs(LutPC), LutGroups, LutGroups, 1);

        // Only AtmosphereCompositePass reads the LUT, and it is a dispatch.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

        AtmosphereTerms.AerialInScatterIndex     = (uint32)InScatter.GetResourceID();
        AtmosphereTerms.AerialTransmittanceIndex = (uint32)Transmittance.GetResourceID();
        AtmosphereTerms.AerialRange              = Range;
        AtmosphereTerms.AerialIntensity          = Frame.Volumetrics.AerialIntensity;
    }
}
