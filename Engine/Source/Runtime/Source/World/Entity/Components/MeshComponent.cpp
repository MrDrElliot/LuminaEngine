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

        // Already transient, and wrapping again adds a chain level per call up to the depth limit.
        if (CMaterialInstance* Existing = Cast<CMaterialInstance>(Current); Existing != nullptr && !Existing->IsAsset())
        {
            return Existing;
        }

        return CMaterialInstance::CreateDynamic(Current);
    }

    void SMeshComponent::InvalidateRenderResolve()
    {
        // Invalidates THIS COMPONENT's copy; the shared entry goes through InvalidateDependency.
        CachedEntryState = MESH_RESOLVE_STATE_STALE;
        FMeshResolveCache::MarkPendingWork();
    }
}
