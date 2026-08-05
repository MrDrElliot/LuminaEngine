#include "RuntimePCH.h"
#include "FoliageComponent.h"
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

        BakedInstances.clear();
        BakedInstances.reserve(Instances.size());
        bBakeIncomplete = false;

        for (const SFoliageInstance& Inst : Instances)
        {
            if (!IsValidType(Inst.TypeIndex))
            {
                continue;
            }
            const SFoliageType& Type = Types[Inst.TypeIndex];
            if (!Type.Mesh.IsValid())
            {
                bBakeIncomplete = true; // the type has no mesh assigned yet (or it's still loading)
                continue;
            }

            const FAABB& LocalBounds = Type.Mesh->GetAABB();
            if (LocalBounds.Max.x < LocalBounds.Min.x)
            {
                // Mesh geometry not resident yet; skip and rebake once it loads.
                bBakeIncomplete = true;
                continue;
            }

            const FMatrix4 Transform = Inst.GetMatrix();
            const FAABB    WorldBox  = LocalBounds.ToWorld(Transform);
            const FVector3 Center    = (WorldBox.Min + WorldBox.Max) * 0.5f;
            const float    Radius    = Math::Length(WorldBox.Max - Center);

            FFoliageBakedInstance& Baked = BakedInstances.emplace_back();
            Baked.Transform    = Transform;
            Baked.SphereBounds = FVector4(Center, Radius);
            Baked.TypeIndex    = Inst.TypeIndex;
        }

        BakedVersion = InstancesVersion;
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
