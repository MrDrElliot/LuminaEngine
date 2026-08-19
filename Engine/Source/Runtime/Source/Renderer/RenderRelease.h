#pragma once

#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "RHITexture.h"

namespace Lumina::RHI
{
    // What the renderer must hand back once nothing can name it any more. Deliberately nothing but
    // indices and RHI handles: the renderer must never learn what a CObject is, and by the time this is
    // acted on the owner is already gone. Callers fill in only the fields they own.
    struct FRenderRelease
    {
        int32               MaterialSlot = -1;                  // FMaterialManager slot to zero and free
        RHI::FManagedTexture Texture;                           // image + bindless slot to release
    };

    /** Two gates, in this order:
     *
     *  1. EXTRACT liveness -- no frame that still names the resource can be recorded. Half of this is the
     *     caller's guarantee (it posts only once the owning object's last strong reference has dropped, so
     *     nothing game-side can hand the resource to a primitive again); the other half is this queue's,
     *     which holds the token until every extract that was already in flight when it was posted has been
     *     rendered.
     *  2. GPU liveness -- the actual release routes through RHI::Core::Retire, which fences per queue.
     *
     *  Gate 2 already existed; gate 1 did not. Releasing on gate 2 alone is what made an unloaded material
     *  flash the magenta placeholder: the manager slot was zeroed and the texture slot unbound on the game
     *  thread while the retained scene still had one or two frames' worth of primitives naming them.
     *
     *  The counters are GLOBAL rather than per-scene on purpose -- one FMaterialManager is shared across
     *  every world, so a slot can be named by primitives in several render scenes at once. */
    class FRenderReleaseQueue
    {
    public:

        /** Post from the owner's destruction path. Safe on any thread. */
        RUNTIME_API void Post(const FRenderRelease& Release);

        /** Once per frame, before any world extracts. */
        void BeginExtract();

        /** Once per frame, after every world has recorded. Promotes whatever gate 1 now clears. */
        void EndRender();

        /** Shutdown: release everything regardless of gate. The device is idle by then and there is no
         *  next frame to wait for, so holding would just leak. */
        void FlushAll();

        NODISCARD uint32 NumPending() const;

    private:

        // Performs the release itself. Not gated -- callers decide.
        static void ReleaseNow(FRenderRelease& Item);

        struct FPending
        {
            FRenderRelease  Release;
            uint64          Generation = 0;   // ExtractGeneration at Post time
        };

        mutable FMutex      Mutex;
        TVector<FPending>   Pending;

        // Bumped by BeginExtract; the generation of the extract most recently STARTED. Stamping with this
        // (rather than the last COMPLETED one) is what covers an extract that was already running when the
        // token was posted and may have captured the resource.
        uint64              ExtractGeneration = 0;

        // Set by EndRender to the extract generation that has now been rendered. An item clears gate 1
        // once this is strictly greater than its stamp.
        uint64              RenderedGeneration = 0;
    };
}
