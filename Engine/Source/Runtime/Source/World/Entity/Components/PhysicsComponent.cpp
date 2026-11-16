#include "RuntimePCH.h"
#include "PhysicsComponent.h"

#include <atomic>

namespace Lumina
{
    void SDynamicMeshColliderComponent::Commit(TVector<FVector3>&& Positions, TVector<uint32>&& Indices)
    {
        TSharedPtr<FDynamicMeshCollisionInput> Input = MakeShared<FDynamicMeshCollisionInput>();
        Input->Positions = Move(Positions);
        Input->Indices   = Move(Indices);
        AtomicStore(&PendingTriangles, Move(Input));
        (void)std::atomic_ref<uint32>(GeometryVersion).fetch_add(1u, std::memory_order_release);
    }

    TSharedPtr<FDynamicMeshCollisionInput> SDynamicMeshColliderComponent::TakeNewTriangles()
    {
        // The version check spares an untouched collider the exchange's lock.
        const uint32 Version = LoadGeometryVersion();
        if (Version == TakenGeometryVersion)
        {
            return nullptr;
        }
        TakenGeometryVersion = Version;
        return AtomicExchange(&PendingTriangles, TSharedPtr<FDynamicMeshCollisionInput>());
    }

    uint32 SDynamicMeshColliderComponent::LoadGeometryVersion() const
    {
        return std::atomic_ref<uint32>(const_cast<uint32&>(GeometryVersion)).load(std::memory_order_acquire);
    }
}
