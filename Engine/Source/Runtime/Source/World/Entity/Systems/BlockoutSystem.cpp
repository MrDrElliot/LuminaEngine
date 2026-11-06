#include "RuntimePCH.h"
#include "BlockoutSystem.h"

#include "SystemContext.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/BlockoutComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Subsystems/BlockoutMeshBuilder.h"

namespace Lumina
{
    namespace BlockoutSystemInternal
    {
        constexpr uint32 kParallelThreshold = 8;

        void ApplyMaterial(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh)
        {
            if (Mesh.MaterialOverrides.IsEmpty())
            {
                Mesh.MaterialOverrides.SetNum(1);
            }
            Mesh.MaterialOverrides[0] = Shape.Material;
            Shape.BuiltMaterial = Shape.Material.Get();
        }
    }

    void SBlockoutSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    bool SBlockoutSystem::Rebuild(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh, bool bKeepCPUData)
    {
        using BlockoutSystemInternal::ApplyMaterial;

        FBlockoutMeshData Data;
        BlockoutMesh::Build(Shape, Data);

        Shape.BuiltHash = BlockoutMesh::HashParameters(Shape);
        Shape.bBuilt    = true;

        if (Data.IsEmpty())
        {
            Mesh.ClearMesh();
            return false;
        }

        // The override has to land before the commit, which is what resolves the surface's material.
        ApplyMaterial(Shape, Mesh);

        Mesh.bKeepCPUMeshletData = bKeepCPUData;
        Mesh.ClearMesh();
        Mesh.SetPositionsData(reinterpret_cast<const float*>(Data.Positions.GetData()), Data.Positions.Num() * 3);
        Mesh.SetNormalsData(reinterpret_cast<const float*>(Data.Normals.GetData()), Data.Normals.Num() * 3);
        Mesh.SetUVsData(reinterpret_cast<const float*>(Data.UVs.GetData()), Data.UVs.Num() * 2);
        Mesh.SetIndicesData(Data.Indices.GetData(), Data.Indices.Num());

        return Mesh.Commit();
    }

    void SBlockoutSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace BlockoutSystemInternal;

        DirtyScratch.Reset();
        MaterialScratch.Reset();

        ChangedShapes.ForEach([&](ECS::FEntity Entity, const SBlockoutComponent& Shape, const SDynamicMeshComponent& Mesh)
        {
            // A collider added after the build has no meshlet streams to read, so the mesh owes it a rebuild.
            const bool bColliderStarved = !Mesh.bKeepCPUMeshletData && NewColliders.TryGet<SDynamicMeshColliderComponent>(Entity) != nullptr;

            if (!Shape.bBuilt || bColliderStarved || Shape.BuiltHash != BlockoutMesh::HashParameters(Shape))
            {
                DirtyScratch.Add(Entity);
            }
            else if (Shape.BuiltMaterial != Shape.Material.Get())
            {
                MaterialScratch.Add(Entity);
            }
        });

        NewColliders.ForEach([&](ECS::FEntity Entity, const SBlockoutComponent&, const SDynamicMeshComponent& Mesh)
        {
            if (!Mesh.bKeepCPUMeshletData && !DirtyScratch.Contains(Entity))
            {
                DirtyScratch.Add(Entity);
            }
        });

        for (ECS::FEntity Entity : MaterialScratch)
        {
            SDynamicMeshComponent& Mesh = Builds.Get<SDynamicMeshComponent>(Entity);
            ApplyMaterial(Builds.Get<SBlockoutComponent>(Entity), Mesh);
            Mesh.MarkRenderStateDirty();
        }

        if (DirtyScratch.IsEmpty())
        {
            return;
        }

        const auto RebuildOne = [&](uint32 Index)
        {
            const ECS::FEntity Entity = DirtyScratch[Index];

            // The collider builds from the CPU meshlet streams, so only a collided shape pays to keep them.
            const bool bKeepCPUData = NewColliders.TryGet<SDynamicMeshColliderComponent>(Entity) != nullptr;
            Rebuild(Builds.Get<SBlockoutComponent>(Entity), Builds.Get<SDynamicMeshComponent>(Entity), bKeepCPUData);
        };

        Jobs::ParallelForOrSerial((uint32)DirtyScratch.Num(), kParallelThreshold, RebuildOne, 1);
    }
}
