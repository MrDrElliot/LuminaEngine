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
    namespace
    {
        constexpr uint32 kParallelThreshold = 8;

        void ApplyMaterial(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh)
        {
            if (Mesh.MaterialOverrides.empty())
            {
                Mesh.MaterialOverrides.resize(1);
            }
            Mesh.MaterialOverrides[0] = Shape.Material;
            Shape.BuiltMaterial = Shape.Material.Get();
        }
    }

    void SBlockoutSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
        Writes<SBlockoutComponent, SDynamicMeshComponent>();
        Reads<SDynamicMeshColliderComponent>();
    }

    bool SBlockoutSystem::Rebuild(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh, bool bKeepCPUData)
    {
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
        Mesh.SetPositionsData(reinterpret_cast<const float*>(Data.Positions.data()), (int32)Data.Positions.size() * 3);
        Mesh.SetNormalsData(reinterpret_cast<const float*>(Data.Normals.data()), (int32)Data.Normals.size() * 3);
        Mesh.SetUVsData(reinterpret_cast<const float*>(Data.UVs.data()), (int32)Data.UVs.size() * 2);
        Mesh.SetIndicesData(Data.Indices.data(), (int32)Data.Indices.size());

        return Mesh.Commit();
    }

    void SBlockoutSystem::OnUpdate()
    {
        LUMINA_PROFILE_SCOPE();

        const FSystemContext& Context = GetContext();

        TVector<ECS::FEntity> Dirty;
        for (auto&& [Entity, Shape, Mesh] : Context.CreateView<SBlockoutComponent, SDynamicMeshComponent>().Each())
        {
            // A collider added after the build has no meshlet streams to read, so the mesh owes it a rebuild.
            const bool bColliderStarved = !Mesh.bKeepCPUMeshletData
                                       && Context.TryGet<SDynamicMeshColliderComponent>(Entity) != nullptr;

            if (!Shape.bBuilt || bColliderStarved || Shape.BuiltHash != BlockoutMesh::HashParameters(Shape))
            {
                Dirty.push_back(Entity);
                continue;
            }

            if (Shape.BuiltMaterial != Shape.Material.Get())
            {
                ApplyMaterial(Shape, Mesh);
                Mesh.RefreshResolvedMaterials();
            }
        }

        if (Dirty.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = Context.GetRegistry();

        const auto RebuildOne = [&](uint32 Index)
        {
            const ECS::FEntity Entity = Dirty[Index];
            SBlockoutComponent& Shape = Registry.Get<SBlockoutComponent>(Entity);
            SDynamicMeshComponent& Mesh = Registry.Get<SDynamicMeshComponent>(Entity);

            // The collider builds from the CPU meshlet streams, so only a collided shape pays to keep them.
            const bool bKeepCPUData = Registry.TryGet<SDynamicMeshColliderComponent>(Entity) != nullptr;
            Rebuild(Shape, Mesh, bKeepCPUData);
        };

        Task::ParallelForOrSerial((uint32)Dirty.size(), kParallelThreshold, RebuildOne, 1);
    }
}
