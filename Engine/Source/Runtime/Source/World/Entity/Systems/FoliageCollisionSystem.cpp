#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "FoliageCollisionSystem.h"
#include "SystemContext.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/FoliageComponent.h"

namespace Lumina
{
    void SFoliageCollisionSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        Writes<SFoliageComponent>();
        Writes<SystemResource::PhysicsQuery>();
    }

    void SFoliageCollisionSystem::OnUpdate(const SSystemContext& Context)
    {

        LUMINA_PROFILE_SCOPE();

        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();
        if (Scene == nullptr)
        {
            return;
        }

        Context.CreateView<SFoliageComponent>().ForEach([&](ECS::FEntity Entity, SFoliageComponent& Foliage)
        {
            if (Foliage.CollisionBakedVersion == Foliage.InstancesVersion)
            {
                return;
            }

            // Heap rather than scratch, since the instance count is unbounded and one block cannot hold a large foliage set.
            TVector<Physics::FStaticInstanceDesc> Descs;
            Descs.reserve(Foliage.Instances.size());
            bool bSourcesReady = true;

            for (const SFoliageInstance& Inst : Foliage.Instances)
            {
                if (!Foliage.IsValidType(Inst.TypeIndex))
                {
                    continue;
                }
                const SFoliageType& Type = Foliage.Types[Inst.TypeIndex];
                if (!Type.bEnableCollision)
                {
                    continue;
                }

                const CStaticMesh* Mesh = Type.Mesh.Get();
                if (Mesh == nullptr || Mesh->HasAnyFlag(OF_NeedsLoad) || Mesh->GetMeshResource().MeshletData.IsEmpty())
                {
                    bSourcesReady = false;
                    continue;
                }
                const SMeshCollision* Collision = Mesh->GetCollision();

                Physics::FStaticInstanceDesc& Desc = Descs.emplace_back();
                Desc.Position  = Inst.Position;
                Desc.Rotation  = Inst.GetRotationQuat();
                Desc.Scale     = Inst.Scale;
                Desc.Collision = Collision;
                Desc.Mesh      = Collision == nullptr ? Mesh : nullptr;
                Desc.Material = Type.PhysicsMaterial.Get();
                Desc.bConvex  = Type.bConvexCollision;
                Desc.Profile  = Type.CollisionProfile;
            }

            // A source mesh is still loading; retry next frame without stamping the version.
            if (!bSourcesReady)
            {
                return;
            }

            if (Foliage.CollisionGroupID != 0)
            {
                Scene->DestroyStaticBodyGroup(Foliage.CollisionGroupID);
                Foliage.CollisionGroupID = 0;
            }
            if (!Descs.empty())
            {
                Foliage.CollisionGroupID = Scene->CreateStaticBodyGroup(Entity, Descs);
            }
            Foliage.CollisionBakedVersion = Foliage.InstancesVersion;
        });
    }

    void SFoliageCollisionSystem::OnTeardown(const SSystemContext& Context)
    {

        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();

        Context.CreateView<SFoliageComponent>().ForEach([&](SFoliageComponent& Foliage)
        {
            if (Scene != nullptr && Foliage.CollisionGroupID != 0)
            {
                Scene->DestroyStaticBodyGroup(Foliage.CollisionGroupID);
            }
            Foliage.CollisionGroupID = 0;
            Foliage.CollisionBakedVersion = 0;
        });
    }
}
