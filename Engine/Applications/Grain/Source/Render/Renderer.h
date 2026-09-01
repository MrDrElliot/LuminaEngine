#pragma once

#include "World/Camera.h"
#include "World/VoxelSim.h"
#include "World/VoxelWorld.h"

#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUtils.h"

namespace Grain
{
    inline constexpr int32 kBloomLevels = 5;
    inline constexpr int32 kAtrousPasses = 4;

    struct FViewArgs
    {
        RHI::GPUPtr Nodes     = 0;
        RHI::GPUPtr Masks     = 0;
        RHI::GPUPtr Prefix    = 0;
        RHI::GPUPtr Children  = 0;
        RHI::GPUPtr Payload   = 0;
        RHI::GPUPtr SimGrid   = 0;
        RHI::GPUPtr SimCoarse = 0;

        // Seven pointers leave the next float4 at offset 56, which straddles a 16 byte boundary.
        RHI::GPUPtr Pad0      = 0;

        FVector4 CameraPos;
        FVector4 CameraFwd;
        FVector4 CameraRight;
        FVector4 CameraUp;
        FVector4 SunDir;
        FVector4 PrevPos;
        FVector4 PrevFwd;
        FVector4 PrevRight;
        FVector4 PrevUp;
        FVector4 Params;
        FVector4 SimOrigin;

        uint32 Unused0   = 0;
        uint32 Unused1   = 0;
        uint32 DebugMode = 0;
        uint32 bSim      = 0;
    };

    static_assert(sizeof(FViewArgs) == 256, "The Slang mirror expects a packed 240 byte block.");

    // Shared by the temporal, a trous and compose passes, since they all need the same view basis.
    struct FDenoiseArgs
    {
        uint32 IDs[4]   = { 0, 0, 0, 0 };
        uint32 Extra[4] = { 0, 0, 0, 0 };

        FVector4 Params;
        FVector4 CameraPos;
        FVector4 CameraFwd;
        FVector4 CameraRight;
        FVector4 CameraUp;
        FVector4 PrevPos;
        FVector4 PrevFwd;
        FVector4 PrevRight;
        FVector4 PrevUp;
        FVector4 SunDir;
    };

    static_assert(sizeof(FDenoiseArgs) == 192, "The Slang mirror expects a packed 192 byte block.");

    struct FDestroyArgs
    {
        RHI::GPUPtr Nodes     = 0;
        RHI::GPUPtr Masks     = 0;
        RHI::GPUPtr Prefix    = 0;
        RHI::GPUPtr Children  = 0;
        RHI::GPUPtr SimGrid   = 0;
        RHI::GPUPtr SimCoarse = 0;
        RHI::GPUPtr Pick      = 0;

        // Seven pointers leave the next float4 at offset 56, which straddles a 16 byte boundary.
        RHI::GPUPtr Pad0      = 0;

        FVector4 Origin;
        FVector4 Direction;
        FVector4 SimOrigin;
        FVector4 Params;
    };

    static_assert(sizeof(FDestroyArgs) == 128, "The Slang mirror expects a packed 112 byte block.");

    class FRenderer
    {
    public:

        bool Initialize(EFormat InSwapchainFormat);
        void Shutdown();

        void EnsureTargets(const FUIntVector2& Extent);
        void SetDebugMode(uint32 Mode) { DebugMode = Mode; }

        void SetTemporal(bool bEnabled) { bTemporal = bEnabled; }
        NODISCARD bool IsTemporalEnabled() const { return bTemporal; }

        void SetFilter(bool bEnabled) { bFilter = bEnabled; }
        NODISCARD bool IsFilterEnabled() const { return bFilter; }

        // Queued rather than issued, because destruction has to land before the frame's raymarch.
        void RequestDestroy(float Radius) { PendingDestroy = Radius; }

        void Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                    const FVoxelWorld& World, const FVoxelSim& Sim, const FCamera& Camera,
                    float RealTime, bool bMoved);

        bool CaptureToFile(const FUIntVector2& Extent, const char* Path);

        void EnableGpuTimers();
        void ReportGpuTimers() const;

    private:

        bool CreatePipelines();
        void ReleaseTargets();

        void FillViewBasis(FDenoiseArgs& Args, const FUIntVector2& Extent, const FCamera& Camera) const;

        void StepSim(RHI::FCmdListH CL, const FVoxelSim& Sim);
        void RunDestroy(RHI::FCmdListH CL, const FVoxelWorld& World, const FVoxelSim& Sim, const FCamera& Camera);

        void DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, const FVoxelWorld& World,
                       const FVoxelSim& Sim, const FCamera& Camera, float RealTime);
        void Accumulate(RHI::FCmdListH CL, const FUIntVector2& Extent, const FCamera& Camera, bool bMoved);
        void FilterIndirect(RHI::FCmdListH CL, const FUIntVector2& Extent);
        void Compose(RHI::FCmdListH CL, const FUIntVector2& Extent, const FCamera& Camera);

        void DrawBloom(RHI::FCmdListH CL);
        void DrawComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent);

        EFormat SwapchainFormat = EFormat::UNKNOWN;

        RHI::FPipelineH RaymarchPipeline;
        RHI::FPipelineH TemporalPipeline;
        RHI::FPipelineH AtrousPipeline;
        RHI::FPipelineH ComposePipeline;
        RHI::FPipelineH DownsamplePipeline;
        RHI::FPipelineH UpsamplePipeline;
        RHI::FPipelineH CompositePipeline;
        RHI::FPipelineH SimStepPipeline;
        RHI::FPipelineH SimCoarsePipeline;
        RHI::FPipelineH PickPipeline;
        RHI::FPipelineH DestroyPipeline;

        void Mark(RHI::FCmdListH CL, uint32 Slot);

        RHI::FQueryPoolH TimerPool;
        bool             bTimers = false;

        RHI::FGPUAllocation PickBuffer;
        RHI::FDepthStencilH DepthState;

        RHI::FManagedTexture RawIndirect;
        RHI::FManagedTexture RawDirect;
        RHI::FManagedTexture RawAlbedo;
        RHI::FManagedTexture Accum[2];
        RHI::FManagedTexture Moment[2];
        RHI::FManagedTexture Scratch[2];
        RHI::FManagedTexture SceneTarget;
        RHI::Utils::FMipChain BloomChain;

        FUIntVector2 TargetExtent { 0, 0 };
        uint32       FrameIndex = 0;
        uint32       DebugMode = 0;
        bool         bTemporal = true;
        bool         bFilter = true;
        float        PendingDestroy = 0.0f;
        int32        WriteIndex = 0;
        int32        FilterOutput = 0;
        bool         bHasHistory = false;

        FVector3 PrevPosition { 0.0f, 0.0f, 0.0f };
        FVector3 PrevForward { 0.0f, 0.0f, 1.0f };
        FVector3 PrevRight { 1.0f, 0.0f, 0.0f };
        FVector3 PrevUp { 0.0f, 1.0f, 0.0f };
    };
}
