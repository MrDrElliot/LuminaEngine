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

    void CBlockoutSystem::Configure()
    {
        QueuesNoCommands();

        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    bool CBlockoutSystem::Rebuild(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh, SDynamicMeshColliderComponent* Collider)
    {
        using BlockoutSystemInternal::ApplyMaterial;

        FBlockoutMeshData Data;
        BlockoutMesh::Build(Shape, Data);

        Shape.BuiltHash = BlockoutMesh::HashParameters(Shape);
        Shape.bBuilt    = true;

        if (Data.IsEmpty())
        {
            Mesh.ClearMesh();
            if (Collider != nullptr)
            {
                Collider->Commit({}, {});
            }
            return false;
        }

        // The override has to land before the commit, which is what resolves the surface's material.
        ApplyMaterial(Shape, Mesh);

        Mesh.ClearMesh();
        Mesh.SetPositionsData(reinterpret_cast<const float*>(Data.Positions.GetData()), Data.Positions.Num() * 3);
        Mesh.SetNormalsData(reinterpret_cast<const float*>(Data.Normals.GetData()), Data.Normals.Num() * 3);
        Mesh.SetUVsData(reinterpret_cast<const float*>(Data.UVs.GetData()), Data.UVs.Num() * 2);
        Mesh.SetIndicesData(Data.Indices.GetData(), Data.Indices.Num());

        // The setters copied the streams, so the collider takes them outright.
        if (Collider != nullptr)
        {
            Collider->Commit(Move(Data.Positions), Move(Data.Indices));
        }

        return Mesh.Commit();
    }

    void CBlockoutSystem::GatherDirty()
    {
        DirtyScratch.Reset();
        ColliderScratch.Reset();
        MaterialScratch.Reset();
        DirtyLookup.BeginPass(ChangedShapes.View(), NewColliders.View());

        ChangedShapes.ForEach([&](ECS::FEntity Entity, const SBlockoutComponent& Shape, const SDynamicMeshComponent&)
        {
            if (!Shape.bBuilt || Shape.BuiltHash != BlockoutMesh::HashParameters(Shape))
            {
                DirtyScratch.Add(Entity);
                DirtyLookup.ByEntityIndex[Entity.GetIndex()] = { Entity, DirtyLookup.Stamp };
            }
            else if (Shape.BuiltMaterial != Shape.Material.Get())
            {
                MaterialScratch.Add(Entity);
            }
        });

        // A rebuild already commits the collider, and so does a caller that built with it in place.
        NewColliders.ForEach([&](ECS::FEntity Entity, const SBlockoutComponent&, const SDynamicMeshComponent&)
        {
            const SDynamicMeshColliderComponent& Collider = NewColliders.Get<SDynamicMeshColliderComponent>(Entity);
            if (DirtyLookup.Find(Entity) == nullptr && Collider.LoadGeometryVersion() == 0)
            {
                ColliderScratch.Add(Entity);
            }
        });
    }

    void CBlockoutSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace BlockoutSystemInternal;

        GatherDirty();

        for (ECS::FEntity Entity : MaterialScratch)
        {
            SDynamicMeshComponent& Mesh = Builds.Get<SDynamicMeshComponent>(Entity);
            ApplyMaterial(Builds.Get<SBlockoutComponent>(Entity), Mesh);
            Mesh.MarkRenderStateDirty();
        }

        for (ECS::FEntity Entity : ColliderScratch)
        {
            CommitCollision(Builds.Get<SBlockoutComponent>(Entity), Colliders.Get<SDynamicMeshColliderComponent>(Entity));
        }

        if (DirtyScratch.IsEmpty())
        {
            return;
        }

        const auto RebuildOne = [&](uint32 Index)
        {
            const ECS::FEntity Entity = DirtyScratch[Index];
            Rebuild(Builds.Get<SBlockoutComponent>(Entity), Builds.Get<SDynamicMeshComponent>(Entity), Colliders.TryGet<SDynamicMeshColliderComponent>(Entity));
        };

        Jobs::ParallelForOrSerial((uint32)DirtyScratch.Num(), kParallelThreshold, RebuildOne, 1);
    }

    void CBlockoutSystem::CommitCollision(const SBlockoutComponent& Shape, SDynamicMeshColliderComponent& Collider)
    {
        FBlockoutMeshData Data;
        BlockoutMesh::Build(Shape, Data);
        Collider.Commit(Move(Data.Positions), Move(Data.Indices));
    }
}
