#include "RuntimePCH.h"
#include "FoliageComponent.h"
#include "World/ECS/Registry.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "World/World.h"

namespace Lumina
{
    const SMeshCollision* SFoliageType::GetCollision() const
    {
        const CStaticMesh* StaticMesh = Mesh.Get();
        return StaticMesh != nullptr ? StaticMesh->GetCollision() : nullptr;
    }

    int32 SFoliageComponent::RemoveInRadius(const FVector3& WorldCenter, float Radius, int32 TypeFilter)
    {
        const float RadiusSq = Radius * Radius;

        int32 Removed = 0;
        int32 Write = 0;
        for (int32 Read = 0; Read < Instances.Num(); ++Read)
        {
            const SFoliageInstance& Inst = Instances[Read];

            const bool bTypeMatch = (TypeFilter < 0) || (Inst.TypeIndex == TypeFilter);
            const float Dx = Inst.Position.x - WorldCenter.x;
            const float Dz = Inst.Position.z - WorldCenter.z;
            const bool bInRadius = (Dx * Dx + Dz * Dz) <= RadiusSq;

            if (bTypeMatch && bInRadius)
            {
                ++Removed;
                continue; // drop it
            }

            if (Write != Read)
            {
                Instances[Write] = Inst;
                // Hidden flags are indexed like Instances, so they close the same holes.
                if (Read < HiddenInstances.Num())
                {
                    HiddenInstances[Write] = HiddenInstances[Read];
                }
                else if (Write < HiddenInstances.Num())
                {
                    HiddenInstances[Write] = 0;
                }
            }
            ++Write;
        }

        Instances.SetNum(Write);
        if (HiddenInstances.Num() > Write)
        {
            HiddenInstances.SetNum(Write);
        }
        if (Removed > 0)
        {
            MarkInstancesChanged();
        }
        return Removed;
    }

    void SFoliageComponent::EnsureRenderCache()
    {
        if (BakedVersion == InstancesVersion && !bBakeIncomplete && !bBakeReleased)
        {
            return; // cache already valid for the current instance set
        }

        // Both rejections are properties of the TYPE, so they resolve once per type instead of per instance.
        const uint32 NumTypes = (uint32)Types.Num();
        BakeTypeBounds.SetNum(NumTypes);
        for (uint32 t = 0; t < NumTypes; ++t)
        {
            const SFoliageType& Type = Types[t];
            BakeTypeBounds[t] = Type.Mesh.IsValid() ? Type.Mesh->GetAABB() : SAABB(FVector3(1.0f), FVector3(-1.0f));
        }
        const auto IsTypeReady = [this](int32 TypeIndex)
        {
            return BakeTypeBounds[TypeIndex].Max.x >= BakeTypeBounds[TypeIndex].Min.x;
        };

        const uint32 NumInstances = (uint32)Instances.Num();

        // Written by index, so the per-instance math can fan out and a skipped instance keeps its index as a hole.
        BakedInstances.SetNum(NumInstances);

        std::atomic<bool> bIncomplete{ false };

        const auto BakeOne = [&](uint32 Index)
        {
            const SFoliageInstance& Inst  = Instances[Index];
            FFoliageBakedInstance&  Baked = BakedInstances[Index];
            if (!IsValidType(Inst.TypeIndex) || !IsTypeReady(Inst.TypeIndex))
            {
                // A valid type whose mesh is unassigned or not resident yet rebakes once it loads.
                if (IsValidType(Inst.TypeIndex))
                {
                    bIncomplete.store(true, std::memory_order_relaxed);
                }
                Baked.Transform    = FMatrix4(1.0f);
                Baked.SphereBounds = FVector4(0.0f);
                Baked.TypeIndex    = -1;
                return;
            }

            const FMatrix4 Transform = Inst.GetMatrix();
            const SAABB    WorldBox  = BakeTypeBounds[Inst.TypeIndex].ToWorld(Transform);
            const FVector3 Center    = (WorldBox.Min + WorldBox.Max) * 0.5f;

            Baked.Transform    = Transform;
            Baked.SphereBounds = FVector4(Center, Math::Length(WorldBox.Max - Center));
            Baked.TypeIndex    = Inst.TypeIndex;
        };

        constexpr uint32 kParallelThreshold = 2048;
        Jobs::ParallelForOrSerial(NumInstances, kParallelThreshold, BakeOne, 256);

        bBakeIncomplete = bIncomplete.load(std::memory_order_relaxed);
        bBakeReleased   = false;
        BakedVersion    = InstancesVersion;
    }

    void SFoliageComponent::PostEditChange(const FPropertyChangedEvent& Event)
    {
        MarkInstancesChanged();
        for (SFoliageType& Type : Types)
        {
            Type.CachedEntryState = MESH_RESOLVE_STATE_STALE;
        }
        FMeshResolveCache::RequestFullSweep();
    }

    void MarkFoliageChanged(CWorld& World, ECS::FEntity Entity, SFoliageComponent& Foliage)
    {
        Foliage.MarkInstancesChanged();

        ECS::FRegistry& Registry = World.GetEntityRegistry();

        // A type owns its own mesh reference, so nothing else queues the resolve it needs.
        FMeshResolveQueue::MarkFoliageDirty(Registry, Entity);

        // Membership as well as Data, because painting changes the instance COUNT the set keys on.
        FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage,
                                                   EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
    }

    void SetFoliageInstanceHidden(CWorld& World, ECS::FEntity Entity, SFoliageComponent& Foliage, uint32 Index, bool bHidden)
    {
        if (Index >= (uint32)Foliage.Instances.Num() || Foliage.IsInstanceHidden(Index) == bHidden)
        {
            return;
        }
        if (Foliage.HiddenInstances.Num() < Foliage.Instances.Num())
        {
            Foliage.HiddenInstances.SetNum(Foliage.Instances.Num(), 0);
        }
        Foliage.HiddenInstances[Index] = bHidden ? 1 : 0;
        FRenderDirtyTracker::Ensure(World.GetEntityRegistry()).MarkInstanceVisibility(Entity, Index);
    }
}
