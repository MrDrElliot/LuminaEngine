#include "RHITestHarness.h"

#include "Memory/MemoryTracking.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHIUpload.h"

namespace Lumina::RHITests
{
    namespace
    {
        // Category 0 is "Default", so untagged allocations are counted too.
        uint64 TotalAllocsAcrossCategories()
        {
            Memory::FMemoryCategoryStats Stats[256];
            const uint32 Count = Memory::GetCategoryStats(Stats, 256);

            uint64 Total = 0;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                Total += Stats[Index].TotalAllocs;
            }
            return Total;
        }

        void RecordRepresentativeFrame(FTestContext& Ctx, RHI::FGPUAllocation Buffer, RHI::FTextureH Texture)
        {
            const RHI::FCmdListH CL = Ctx.OpenCL();

            RHI::CmdBeginMarker(CL, "AllocProbe");
            RHI::Barriers::AllToTransfer(CL);
            RHI::CmdMemzero(CL, Buffer.Gpu, 4096);

            const float Clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            RHI::CmdClearTexture(CL, Texture, Clear);
            RHI::Barriers::TransferToAll(CL);

            const uint32 Payload[4] = { 1, 2, 3, 4 };
            const RHI::GPUPtr Args = RHI::Core::CopyTransientArray(Payload, 4);
            RHI::CmdSetSceneRoot(CL, RHI::FSceneBindings{ .Root = Args });

            RHI::CmdEndMarker(CL);
            Ctx.SubmitAndWait(CL);
        }
    }

    // A steady-state frame runs off the transient ring, the scratch arena and pooled command lists.
    RHI_TEST(Allocations, PerFramePathsDoNotAllocate)
    {
        const RHI::FGPUAllocation Buffer = Ctx.Malloc(4096, RHI::EMemoryType::GPUOnly, "AllocProbe.Buffer");
        RHI_REQUIRE(Buffer.Gpu != 0);

        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(64), "AllocProbe.Texture");
        RHI_REQUIRE(RHI::IsValid(Texture));

        constexpr uint32 kWarmIterations     = 32;
        constexpr uint32 kMeasuredIterations = 64;

        // A budget above zero only where a container legitimately doubles its way to steady state.
        auto Measure = [&](const char* Label, uint64 Budget, auto&& Body)
        {
            for (uint32 Warm = 0; Warm < kWarmIterations; ++Warm)
            {
                Body();
            }

            const uint64 Before = TotalAllocsAcrossCategories();
            for (uint32 Iteration = 0; Iteration < kMeasuredIterations; ++Iteration)
            {
                Body();
            }
            const uint64 Delta = TotalAllocsAcrossCategories() - Before;

            if (Delta > Budget)
            {
                Ctx.Failf("%s allocated %llu times over %u iterations (budget %llu)",
                          Label, (unsigned long long)Delta, kMeasuredIterations, (unsigned long long)Budget);
            }
        };

        Measure("record+submit", 0, [&] { RecordRepresentativeFrame(Ctx, Buffer, Texture); Ctx.PumpFrames(1); });

        Measure("frame pump", 0, [&] { Ctx.PumpFrames(1); });

        Measure("transient ring", 0, [&]
        {
            const uint32 Payload[4] = { 1, 2, 3, 4 };
            (void)RHI::Core::CopyTransientArray(Payload, 4);
            Ctx.PumpFrames(1);
        });

        Measure("retire+drain", 0, [&]
        {
            const RHI::FGPUAllocation Scratch = RHI::Malloc(256, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::Core::Retire(Scratch);
            Ctx.PumpFrames(RHI::kFramesInFlight + 1);
        });

        // The in-flight batch list doubles to steady state; the flush itself is heap-free.
        Measure("texture upload", 4, [&]
        {
            const uint32 Pixels[16] = {};
            RHI::UploadTexture(Texture, 0, 0, Pixels, sizeof(Pixels), 4);
            Ctx.PumpFrames(1);
        });
    }
}
