#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererImpostorInternal
    {
        struct FCullImpostorPointsPC
        {
            RHI::TGPUSpan<FImpostorPoint>       Points;
            RHI::TGPUSpan<FImpostorType>        Types;
            RHI::TGPUSpan<FInstanceBlockBounds> Blocks;
            RHI::TGPUSpan<uint32>               OutList;
            RHI::TGPUSpan<uint32>               OutCount;
            uint32                              NumViews   = 0u;
            uint32                              FirstLevel = 0u;
            uint32                              LastLevel  = 0u;
            uint32                              _Pad0      = 0u;
        };
        static_assert(sizeof(FCullImpostorPointsPC) == 96, "FCullImpostorPointsPC must match CullImpostorPoints.slang.");

        struct FImpostorShadowPC
        {
            RHI::TGPUSpan<uint32> List;
            int32                 ShadowDataBase = 0;
            uint32                _Pad0 = 0u;
            uint32                _Pad1 = 0u;
            uint32                _Pad2 = 0u;
        };
        static_assert(sizeof(FImpostorShadowPC) == 32, "FImpostorShadowPC must match Includes/ImpostorShadow.slang.");

        // Room for this many casters per point before last frame's demand says otherwise, since a fresh map lists a point once per level it reaches.
        constexpr uint32 kInitialShadowEntriesPerPoint = 2u;
        constexpr uint32 kMinShadowEntries = 1u << 16;

        FUIntVector2 FoldedPointGroups(uint32 Points)
        {
            return RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(Points, IMPOSTOR_POINT_GROUP_SIZE));
        }
    }

    void FDefaultSceneRenderer::UploadImpostorPoints(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload)
    {
        const FImpostorPointSet& Set = ScenePrimitives.GetImpostorPoints();
        const uint32 Points = Upload.ImpostorPointCount;
        const uint32 Types  = Upload.ImpostorTypeCount;
        const uint32 Blocks = Upload.ImpostorBlockCount;

        ImpostorPointsLive = 0u;
        SceneRootShared.ImpostorPoints = {};
        SceneRootShared.ImpostorTypes  = {};
        if (Points == 0u || Types == 0u || Blocks == 0u || Set.GetPointCount() < Points || Set.GetTypeCount() < Types || Set.GetBlockCount() < Blocks)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Impostor Point Upload");

        const bool bWrites = !Upload.ImpostorPointRuns.IsEmpty() || !Upload.ImpostorTypeRuns.IsEmpty() || !Upload.ImpostorBlockRuns.IsEmpty();
        if (bWrites)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
        }

        // Kept across a grow, since only what changed is sent.
        ReserveBuffer(CL, ImpostorPointBuffer, (uint64)Points * sizeof(FImpostorPoint), /*bAllowShrink*/ true, /*bPreserveContents*/ true);
        ReserveBuffer(CL, ImpostorTypeBuffer,  (uint64)Types * sizeof(FImpostorType),   /*bAllowShrink*/ true, /*bPreserveContents*/ true);
        ReserveBuffer(CL, ImpostorBlockBuffer, (uint64)Blocks * sizeof(FInstanceBlockBounds), /*bAllowShrink*/ true, /*bPreserveContents*/ true);
        const bool bFits = ImpostorPointBuffer.CapacityOf<FImpostorPoint>() >= Points
                        && ImpostorTypeBuffer.CapacityOf<FImpostorType>() >= Types
                        && ImpostorBlockBuffer.CapacityOf<FInstanceBlockBounds>() >= Blocks;
        if (!bFits)
        {
            bImpostorResendAll.store(true, std::memory_order_release);
            return;
        }

        // Copied now rather than by the deferred fills, since the next sync rewrites and resizes these arrays without waiting on them.
        WriteBufferRuns(CL, ImpostorPointBuffer.Gpu, Set.GetPoints(), sizeof(FImpostorPoint), Upload.ImpostorPointRuns);
        WriteBufferRuns(CL, ImpostorTypeBuffer.Gpu, Set.GetTypes(), sizeof(FImpostorType), Upload.ImpostorTypeRuns);
        WriteBufferRuns(CL, ImpostorBlockBuffer.Gpu, Set.GetBlockBounds(), sizeof(FInstanceBlockBounds), Upload.ImpostorBlockRuns);

        ImpostorPointsLive = Points;
        SceneRootShared.ImpostorPoints = RHI::TGPUSpan<FImpostorPoint>{ ImpostorPointBuffer, Points };
        SceneRootShared.ImpostorTypes  = RHI::TGPUSpan<FImpostorType>{ ImpostorTypeBuffer, Types };
        LUMINA_PROFILE_VALUE("Impostor/PointSlots", (int64)Points);
    }

    void FDefaultSceneRenderer::DispatchCullImpostorPoints(RHI::FCmdListH CL, const FFrameData& Frame, RHI::TGPUSpan<uint32> OutProxyList, RHI::FGPURange OutProxyCount)
    {
        using namespace SceneRendererImpostorInternal;

        static const FShaderH CullShader = FShaderLibrary::Get("CullImpostorPoints.slang");
        if (CullShader == nullptr || ImpostorPointsLive == 0u || OutProxyList.Count == 0u)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Cull Impostor Points");

        const uint32 Points = ImpostorPointsLive;
        FCullImpostorPointsPC PC;
        PC.Points   = { ImpostorPointBuffer, Points };
        PC.Types    = SceneRootShared.ImpostorTypes;
        PC.Blocks   = { ImpostorBlockBuffer, RenderUtils::GetGroupCount(Points, IMPOSTOR_POINT_GROUP_SIZE) };
        PC.OutList  = OutProxyList;
        PC.OutCount = RHI::TGPUSpan<uint32>::FromAddress(OutProxyCount.Address, 1u);
        PC.NumViews = (uint32)Frame.Views.CullViews.Num();
        const FUIntVector2 Grid = FoldedPointGroups(Points);
        DispatchCompute(CL, CullShader, PC, Grid.x, Grid.y);
    }

    void FDefaultSceneRenderer::CullImpostorShadows(RHI::FCmdListH CL)
    {
        using namespace SceneRendererImpostorInternal;

        bImpostorShadowsListed = false;
        static const FString  ShadowDefine = "IMPOSTOR_SHADOW_CULL";
        static const FShaderH CullShader   = FShaderLibrary::Get("CullImpostorPoints.slang", TSpan<const FString>(&ShadowDefine, 1));
        const auto& Virtual = RenderFrame->Lighting.VirtualShadow;
        if (CullShader == nullptr || ImpostorPointsLive == 0u || !Virtual.bActive || Virtual.LastLevel < Virtual.FirstLevel)
        {
            return;
        }

        // This slot's last frame finished before the slot came round again, so its count is settled.
        RHI::FGPUAllocation& Readback = ImpostorShadowReadback[CurrentFrameSlot];
        if (Readback.Gpu == 0)
        {
            Readback = RHI::Malloc(sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Readback.Gpu, "Readback.ImpostorShadows");
            if (Readback.Cpu != nullptr)
            {
                Memory::Memzero(Readback.Cpu, sizeof(uint32));
            }
        }
        else if (const uint32* Demand = Readback.CpuAs<const uint32>())
        {
            LUMINA_PROFILE_VALUE("Impostor/ShadowCasters", (int64)*Demand);
            // Casters past the list were never drawn into pages that will not draw again, so a short list costs a full redraw at the larger size.
            if (*Demand > ImpostorShadowCapacity && ImpostorShadowCapacity != 0u)
            {
                ImpostorShadowDemand = *Demand + *Demand / 4u;
                SunShadowCache.RequestInvalidateAll();
            }
        }

        const uint32 Wanted = Math::Max(Math::Max(ImpostorShadowDemand, ImpostorPointsLive * kInitialShadowEntriesPerPoint), kMinShadowEntries);
        ReserveBuffer(CL, ImpostorShadowList, (uint64)Wanted * sizeof(uint32), /*bAllowShrink*/ false);
        ReserveBuffer(CL, ImpostorShadowArgs, sizeof(RHI::FDrawIndirectArguments), /*bAllowShrink*/ false);
        ImpostorShadowCapacity = ImpostorShadowList.CapacityOf<uint32>();
        if (!ImpostorShadowList || !ImpostorShadowArgs || ImpostorShadowCapacity == 0u)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Cull Impostor Shadows");

        const RHI::FDrawIndirectArguments Args = { 4u, 0u, 0u, 0u };
        WriteBuffer(CL, ImpostorShadowArgs.Gpu, &Args, sizeof(Args));
        Barriers::TransferToCompute(CL);

        const uint32 Points = ImpostorPointsLive;
        FCullImpostorPointsPC PC;
        PC.Points     = { ImpostorPointBuffer, Points };
        PC.Types      = SceneRootShared.ImpostorTypes;
        PC.Blocks     = { ImpostorBlockBuffer, RenderUtils::GetGroupCount(Points, IMPOSTOR_POINT_GROUP_SIZE) };
        PC.OutList    = { ImpostorShadowList, ImpostorShadowCapacity };
        PC.OutCount   = RHI::TGPUSpan<uint32>::FromAddress(ImpostorShadowArgs.Gpu + offsetof(RHI::FDrawIndirectArguments, InstanceCount), 1u);
        PC.FirstLevel = Virtual.FirstLevel;
        PC.LastLevel  = Virtual.LastLevel;
        const FUIntVector2 Grid = FoldedPointGroups(Points);
        DispatchCompute(CL, CullShader, PC, Grid.x, Grid.y);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::VertexShader | RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::IndirectRead | RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::TransferRead);
        RHI::CmdMemcpy(CL, { Readback.Gpu, sizeof(uint32) }, { ImpostorShadowArgs.Gpu + offsetof(RHI::FDrawIndirectArguments, InstanceCount), sizeof(uint32) });
        bImpostorShadowsListed = true;
    }

    void FDefaultSceneRenderer::DrawImpostorShadows(RHI::FCmdListH CL)
    {
        using namespace SceneRendererImpostorInternal;

        static const FShaderH VertexShader = FShaderLibrary::Get("ImpostorShadowVert.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("ImpostorShadowPixel.slang");
        const auto& Virtual = RenderFrame->Lighting.VirtualShadow;
        if (!bImpostorShadowsListed || VertexShader == nullptr || PixelShader == nullptr)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Impostor Shadows");

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::UNKNOWN;
        Key.Topology    = RHI::ETopology::TriangleStrip;
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Every level covers the whole virtual map, and the pixel shader keeps only what lands in a static page drawing this frame.
        const RHI::FRect Full{ 0, (int32)VSM_VIRTUAL_SIZE, 0, (int32)VSM_VIRTUAL_SIZE };
        RHI::CmdSetViewport(CL, Full);
        RHI::CmdSetScissor(CL, Full);

        FImpostorShadowPC PC;
        PC.List           = { ImpostorShadowList, ImpostorShadowCapacity };
        PC.ShadowDataBase = Virtual.ShadowDataBase;
        RHI::CmdDrawIndirect(CL, MakeArgs(PC), ImpostorShadowArgs.Sub(0u, sizeof(RHI::FDrawIndirectArguments)), 1u, sizeof(RHI::FDrawIndirectArguments));
    }
}
