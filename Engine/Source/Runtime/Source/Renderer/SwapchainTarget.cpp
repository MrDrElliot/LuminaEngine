#include "SwapchainTarget.h"

#include "RHICore.h"

namespace Lumina::RHI
{
    FSwapchainTarget::~FSwapchainTarget()
    {
        Shutdown();
    }

    void FSwapchainTarget::Initialize(FSurfaceH InSurface, const FUIntVector2& Extent)
    {
        if (!IsValid(InSurface))
        {
            return;
        }

        Surface = InSurface;
        Resize(Extent);
    }

    void FSwapchainTarget::Shutdown()
    {
        if (IsValid(Swapchain))
        {
            // Recording is synchronous, so only submitted work can still name these images.
            WaitDeviceIdle();
            FreeH(Swapchain);
        }
        else if (IsValid(Surface))
        {
            // The first build consumes the surface, so this only runs when nothing was ever built.
            FreeH(Surface);
        }

        Swapchain       = FSwapchainH{};
        Surface         = FSurfaceH{};
        RequestedExtent = FUIntVector2(0, 0);
        BuiltExtent     = FUIntVector2(0, 0);
    }

    void FSwapchainTarget::Resize(const FUIntVector2& Extent)
    {
        if (Extent.x == 0 || Extent.y == 0)
        {
            return;   // minimized or mid-drag, so the last good request stands until it has area again
        }

        RequestedExtent = Extent;
        if (RequestedExtent != BuiltExtent)
        {
            Recreate();
        }
    }

    void FSwapchainTarget::Recreate()
    {
        if (RequestedExtent.x == 0 || RequestedExtent.y == 0)
        {
            return;
        }

        if (IsValid(Swapchain))
        {
            RecreateSwapchain(Swapchain, RequestedExtent);
        }
        else if (IsValid(Surface))
        {
            Swapchain = CreateSwapchain(Surface, RequestedExtent);
            Surface   = FSurfaceH{};
        }
        else
        {
            return;
        }

        // A surface with no drawable area leaves the swapchain unbuilt, and a zero here retries next frame.
        const FUIntVector2 Built = GetSwapchainExtent(Swapchain);
        BuiltExtent = (Built.x == 0 || Built.y == 0) ? FUIntVector2(0, 0) : RequestedExtent;
    }

    FTextureH FSwapchainTarget::Acquire()
    {
        if (!IsValid(Swapchain))
        {
            return {};
        }

        const FTextureH Image = AcquireNextImage(Swapchain);
        if (!IsValid(Image))
        {
            Recreate();
        }
        return Image;
    }

    bool FSwapchainTarget::Present(FCmdListH CL)
    {
        const bool bPresented = Core::Present(Swapchain, CL);
        if (!bPresented)
        {
            // Only the swapchain the image came from can release it, so the rebuild is what frees it.
            Recreate();
        }
        return bPresented;
    }

    void FSwapchainTarget::BarrierToRender(FCmdListH CL)
    {
        CmdSwapchainBarrierToRender(CL, Swapchain);
    }

    FUIntVector2 FSwapchainTarget::GetExtent() const
    {
        return IsValid(Swapchain) ? GetSwapchainExtent(Swapchain) : FUIntVector2(0, 0);
    }

    EFormat FSwapchainTarget::GetFormat() const
    {
        return GetSwapchainFormat(Swapchain);
    }
}
