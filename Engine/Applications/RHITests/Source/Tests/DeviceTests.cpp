#include "RHITestHarness.h"

#include "Log/Log.h"
#include "Renderer/RHITexture.h"

namespace Lumina::RHITests
{
    RHI_TEST(Device, DeviceInfoIsPopulated)
    {
        const RHI::FGPUDeviceInfo Info = RHI::GetDeviceInfo();
        RHI_CHECK(!Info.Name.empty());
        RHI_CHECK(!Info.APIName.empty());
        RHI_CHECK(Info.VendorID != 0);
    }

    RHI_TEST(Device, MeshWorkGroupLimitIsUsable)
    {
        // Every meshlet draw folds its grid against this. Zero would silently drop every draw.
        RHI_CHECK(RHI::GetMaxMeshWorkGroupCount() > 0);
    }

    RHI_TEST(Device, ClampCPUWriteSliceFitsBudget)
    {
        // Asking for more than the BAR heap can hold must come back smaller, not fail and not lie.
        const uint64 Huge = 64ull * 1024 * 1024 * 1024;
        const uint64 Clamped = RHI::ClampCPUWriteSlice("RHITests", Huge, RHI::kFramesInFlight);

        RHI_CHECK(Clamped > 0);
        RHI_CHECK(Clamped <= Huge);
    }

    RHI_TEST(Device, GlobalHeapExists)
    {
        RHI_CHECK(RHI::IsValid(RHI::Core::GetGlobalHeap()));
    }

    RHI_TEST(Device, FallbackTextureIsRegistered)
    {
        // Every unset bindless index resolves through this. Without it a missing texture reads whatever
        // the slot happened to contain.
        RHI_CHECK(RHI::Textures::DefaultResourceID() != RHI::kInvalidHeapSlot);
    }

    RHI_TEST(Device, QueueTimelinesAreDistinct)
    {
        const RHI::FSemaphoreH Graphics = RHI::Core::GetQueueTimeline(RHI::EQueueType::Graphics);
        const RHI::FSemaphoreH Transfer = RHI::Core::GetQueueTimeline(RHI::EQueueType::Transfer);
        const RHI::FSemaphoreH Compute  = RHI::Core::GetQueueTimeline(RHI::EQueueType::Compute);

        RHI_CHECK(RHI::IsValid(Graphics));
        RHI_CHECK(RHI::IsValid(Transfer));
        RHI_CHECK(RHI::IsValid(Compute));
        RHI_CHECK(Graphics.Handle != Transfer.Handle);
        RHI_CHECK(Graphics.Handle != Compute.Handle);
    }

    RHI_TEST(Device, FramePumpIsStable)
    {
        // BeginFrame waits, drains, flushes uploads and recycles command lists. Running it well past a
        // full ring rotation with nothing else going on should be completely inert.
        Ctx.PumpFrames(RHI::kFramesInFlight * 8);
    }
}
