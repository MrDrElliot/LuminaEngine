#include "RuntimePCH.h"
#include "FoliageComponent.h"

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
}
