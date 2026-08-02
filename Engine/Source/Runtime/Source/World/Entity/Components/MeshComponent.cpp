#include "RuntimePCH.h"
#include "MeshComponent.h"

#include "World/Scene/RenderScene/MeshResolveCache.h"

namespace Lumina
{
    void SMeshComponent::InvalidateRenderResolve()
    {
        // 0 never matches a live epoch (the cache starts at 1), so the next gather re-resolves this one.
        CachedEpoch = 0;
        FMeshResolveCache::MarkPendingWork();
    }
}
