#pragma once

#include "RHI.h"
#include "Core/LuminaMacros.h"

namespace Lumina::RHI
{
    // A rejected present keeps the image, so a host that skips the rebuild breaks its next acquire.
    class RUNTIME_API FSwapchainTarget
    {
    public:

        FSwapchainTarget() = default;
        ~FSwapchainTarget();
        LE_NO_COPYMOVE(FSwapchainTarget);

        // Takes ownership of Surface either way. A zero Extent defers the first build to Resize.
        void Initialize(FSurfaceH InSurface, const FUIntVector2& Extent = FUIntVector2(0, 0));
        void Shutdown();

        // Rebuilds only when Extent differs from the last one asked for, so this is cheap every frame.
        void Resize(const FUIntVector2& Extent);

        // Rebuilds at the current extent, for a change the extent cannot express such as present mode.
        void Recreate();

        // An invalid handle means nothing to draw into this frame; skip it, the retry is already armed.
        FTextureH Acquire();

        // Submits CL and presents the image Acquire returned, rebuilding if the present was rejected.
        bool Present(FCmdListH CL);

        // Moves the acquired image into a renderable layout. Recorded before the first pass.
        void BarrierToRender(FCmdListH CL);

        // What the swapchain built at, which a clamped surface can make smaller than the extent asked for.
        FUIntVector2 GetExtent() const;
        EFormat GetFormat() const;

    private:

        FSurfaceH    Surface;                    // consumed by the first build
        FSwapchainH  Swapchain;
        FUIntVector2 RequestedExtent { 0, 0 };
        FUIntVector2 BuiltExtent     { 0, 0 };   // zero until a build succeeds, which arms the retry
    };
}
