#include "RuntimePCH.h"
#include "MeshComponent.h"

#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Core/Object/Cast.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Log/Log.h"

namespace Lumina
{
    CMaterialInstance* MeshComponentUtils::MakeDynamicMaterialInstance(CMaterialInterface* Current)
    {
        if (Current == nullptr)
        {
            LOG_WARN("CreateDynamicMaterialInstance: the slot has no material to instance.");
            return nullptr;
        }

        // Already transient: wrapping again would add a chain level per call, up to the depth limit.
        if (CMaterialInstance* Existing = Cast<CMaterialInstance>(Current); Existing != nullptr && !Existing->IsAsset())
        {
            return Existing;
        }

        return CMaterialInstance::CreateDynamic(Current);
    }

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
