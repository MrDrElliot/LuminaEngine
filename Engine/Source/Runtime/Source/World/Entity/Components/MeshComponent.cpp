#include "RuntimePCH.h"
#include "MeshComponent.h"

#include "World/Scene/RenderScene/MeshResolveCache.h"

namespace Lumina
{
    void SMeshComponent::InvalidateRenderResolve()
    {
        // Bit 0 is set, which no live entry token ever has, so the next resolve pass re-reads this one.
        // Note the scope: this invalidates THIS COMPONENT's copy, not the shared resolve entry. Anything
        // that changes the entry itself (the mesh's GPU buffers landing, a material recompiling) goes
        // through FMeshResolveCache::InvalidateDependency instead.
        CachedEntryState = MESH_RESOLVE_STATE_STALE;
        FMeshResolveCache::MarkPendingWork();
    }
}
