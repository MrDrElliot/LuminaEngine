#include "RuntimePCH.h"
#include "FoliageComponent.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "World/World.h"

namespace Lumina
{
    int32 SFoliageComponent::RemoveInRadius(const FVector3& WorldCenter, float Radius, int32 TypeFilter)
    {
        const float RadiusSq = Radius * Radius;

        int32 Removed = 0;
        size_t Write = 0;
        for (size_t Read = 0; Read < Instances.size(); ++Read)
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
            }
            ++Write;
        }

        Instances.resize(Write);
        if (Removed > 0)
        {
            MarkInstancesChanged();
        }
        return Removed;
    }

    void SFoliageComponent::EnsureRenderCache()
    {
        if (BakedVersion == InstancesVersion && !bBakeIncomplete)
        {
            return; // cache already valid for the current instance set
        }

        // Both rejections are properties of the TYPE, so they resolve once per type instead of per instance.
        const uint32 NumTypes = (uint32)Types.size();
        TVector<uint8> TypeReady;
        TVector<FAABB> TypeBounds;
        TypeReady.assign(NumTypes, 0u);
        TypeBounds.resize(NumTypes);

        for (uint32 t = 0; t < NumTypes; ++t)
        {
            const SFoliageType& Type = Types[t];
            if (!Type.Mesh.IsValid())
            {
                continue;
            }

            const FAABB& LocalBounds = Type.Mesh->GetAABB();
            if (LocalBounds.Max.x < LocalBounds.Min.x)
            {
                continue;
            }

            TypeBounds[t] = LocalBounds;
            TypeReady[t]  = 1u;
        }

        const uint32 NumInstances = (uint32)Instances.size();

        // Written by index rather than appended, so the per-instance transform and bounds math can fan out.
        BakedInstances.resize(NumInstances);

        std::atomic<uint32> SkippedCount{ 0 };
        std::atomic<bool>   bIncomplete{ false };

        const auto BakeOne = [&](uint32 Index)
        {
            const SFoliageInstance& Inst = Instances[Index];
            if (!IsValidType(Inst.TypeIndex))
            {
                SkippedCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            if (TypeReady[Inst.TypeIndex] == 0u)
            {
                // Mesh unassigned or geometry not resident yet; rebake once it loads.
                SkippedCount.fetch_add(1, std::memory_order_relaxed);
                bIncomplete.store(true, std::memory_order_relaxed);
                return;
            }

            const FMatrix4 Transform = Inst.GetMatrix();
            const FAABB    WorldBox  = TypeBounds[Inst.TypeIndex].ToWorld(Transform);
            const FVector3 Center    = (WorldBox.Min + WorldBox.Max) * 0.5f;

            FFoliageBakedInstance& Baked = BakedInstances[Index];
            Baked.Transform    = Transform;
            Baked.SphereBounds = FVector4(Center, Math::Length(WorldBox.Max - Center));
            Baked.TypeIndex    = Inst.TypeIndex;
        };

        constexpr uint32 kParallelThreshold = 2048;
        if (NumInstances < kParallelThreshold || GTaskSystem == nullptr)
        {
            for (uint32 i = 0; i < NumInstances; ++i)
            {
                BakeOne(i);
            }
        }
        else
        {
            Task::ParallelFor(NumInstances, BakeOne, 256);
        }

        // A rejected instance leaves a hole, and only the rejecting case pays for closing them.
        if (SkippedCount.load(std::memory_order_relaxed) != 0)
        {
            uint32 Write = 0;
            for (uint32 Read = 0; Read < NumInstances; ++Read)
            {
                const int32 TypeIndex = Instances[Read].TypeIndex;
                if (!IsValidType(TypeIndex) || TypeReady[TypeIndex] == 0u)
                {
                    continue;
                }
                if (Write != Read)
                {
                    BakedInstances[Write] = BakedInstances[Read];
                }
                ++Write;
            }
            BakedInstances.resize(Write);
        }

        bBakeIncomplete = bIncomplete.load(std::memory_order_relaxed);
        BakedVersion    = InstancesVersion;
    }

    void MarkFoliageChanged(CWorld& World, entt::entity Entity, SFoliageComponent& Foliage)
    {
        Foliage.MarkInstancesChanged();

        // Third cache, and the one that actually makes the instances DRAWABLE. A foliage type owns its own
        // mesh reference, so assigning or swapping it changes nothing about any mesh COMPONENT -- and
        // ResolveDirtyMeshComponents, the only pass that stamps a type's ResolveHandle, does nothing unless
        // the pending-work generation has moved. Without this a painted type never resolves, SyncFoliage
        // creates primitives whose Surfaces stay null, and the foliage is invisible until some unrelated
        // mesh edit happens to bump the generation for us. That is the "paint shows up only after I add
        // another entity" bug.
        FMeshResolveCache::MarkPendingWork();

        // CWorld keeps its registry private; ECS::GetWorldRegistry is the sanctioned accessor, and taking
        // the world here means callers never need one at all.
        FEntityRegistry& Registry = ECS::GetWorldRegistry(World);

        // Membership as well as Data: painting and erasing change the instance COUNT, and the primitive
        // set keys foliage primitives per instance index. Data alone would refresh the ones that already
        // exist and never create or drop any.
        FRenderDirtyTracker::Ensure(Registry).Mark(Entity, EPrimitiveSource::Foliage,
                                                   EPrimitiveDirty::Data | EPrimitiveDirty::Membership);
    }
}
