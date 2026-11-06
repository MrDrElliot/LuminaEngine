#pragma once

#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "RHITexture.h"

namespace Lumina::RHI
{
    // Only indices and RHI handles, since the owning CObject is gone by the time this is acted on.
    struct FRenderRelease
    {
        int32               MaterialSlot = -1;                  // FMaterialManager slot to zero and free
        RHI::FManagedTexture Texture;                           // image + bindless slot to release
        RHI::FGPUAllocation Buffer = {};                        // GPU allocation to retire
    };

    // Retires Block once every extract in flight at the call has been rendered, then on the GPU fence.
    RUNTIME_API void RetireAfterExtract(const FGPUAllocation& Block);

    // Holds each release until every extract in flight when it was posted has rendered, then retires it on the GPU fence.
    class FRenderReleaseQueue
    {
    public:

        // Post from the owner's destruction path. Safe on any thread.
        RUNTIME_API void Post(const FRenderRelease& Release);

        // Once per frame, before any world extracts.
        void BeginExtract();

        // Once per frame, after every world has recorded.
        void EndRender();

        // Releases everything regardless of gate, for shutdown with the device idle.
        void FlushAll();

        NODISCARD uint32 NumPending() const;

        // Performs the release itself. Not gated -- callers decide.
        static void ReleaseNow(FRenderRelease& Item);

    private:

        struct FPending
        {
            FRenderRelease  Release;
            uint64          Generation = 0;   // ExtractGeneration at Post time
        };

        mutable FMutex      Mutex;
        TVector<FPending>   Pending;

        // The most recently started extract, so a stamp also covers one already running when the item was posted.
        uint64              ExtractGeneration = 0;

        // An item is released once this is strictly greater than its stamp.
        uint64              RenderedGeneration = 0;
    };
}
