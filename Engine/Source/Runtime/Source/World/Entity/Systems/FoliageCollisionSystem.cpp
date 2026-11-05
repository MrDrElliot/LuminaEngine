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
    }

    void SFoliageCollisionSystem::OnUpdate(const SSystemContext& Context)
    {
        if (Context.GetPhysics() == nullptr)
        {
            return;
        }

        FoliageSets.ForEach([&](ECS::FEntity Entity, const SFoliageComponent& Source)
        {
            if (Source.CollisionBakedVersion == Source.InstancesVersion)
            {
                return;
            }

            // Heap rather than scratch, since the instance count is unbounded and one block cannot hold a large foliage set.
            TVector<Physics::FStaticInstanceDesc> Descs;
            Descs.Reserve(Source.Instances.Num());
            bool bSourcesReady = true;

            for (int32 InstanceIndex = 0; InstanceIndex < Source.Instances.Num(); ++InstanceIndex)
            {
                const SFoliageInstance& Inst = Source.Instances[InstanceIndex];
                if (!Source.IsValidType(Inst.TypeIndex))
                {
                    continue;
                }
                const SFoliageType& Type = Source.Types[Inst.TypeIndex];
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

                Physics::FStaticInstanceDesc& Desc = Descs.EmplaceGetRef();
                Desc.Position  = Inst.Position;
                Desc.Rotation  = Inst.GetRotationQuat();
                Desc.Scale     = Inst.Scale;
                Desc.Collision = Collision;
                Desc.Mesh      = Collision == nullptr ? Mesh : nullptr;
                Desc.Material = Type.PhysicsMaterial.Get();
                Desc.bConvex  = Type.bConvexCollision;
                Desc.Profile  = Type.CollisionProfile;
                Desc.SourceIndex = InstanceIndex;
            }

            // A source mesh is still loading; retry next frame without stamping the version.
            if (!bSourcesReady)
            {
                return;
            }

            // Swapping bodies edits the physics world, so it lands at the flush rather than under other systems' queries.
            Context.Defer([Entity, Descs = Move(Descs), Version = Source.InstancesVersion](const SExclusiveContext& X) mutable
            {
                Physics::IPhysicsScene* Scene = X.GetPhysicsScene();
                if (Scene == nullptr || !X.IsValidEntity(Entity))
                {
                    return;
                }
                X.Patch<SFoliageComponent>(Entity, [Scene, Entity, Descs = Move(Descs), Version](SFoliageComponent& Foliage)
                {
                    if (Foliage.CollisionGroupID != 0)
                    {
                        Scene->DestroyStaticBodyGroup(Foliage.CollisionGroupID);
                        Foliage.CollisionGroupID = 0;
                    }
                    if (!Descs.IsEmpty())
                    {
                        Foliage.CollisionGroupID = Scene->CreateStaticBodyGroup(Entity, Descs);
                    }
                    Foliage.CollisionBakedVersion = Version;
                });
            });
        });
    }

    void SFoliageCollisionSystem::OnTeardown(const SExclusiveContext& Context)
    {
        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();

        Context.GetRegistry().View<ECS::Write<SFoliageComponent>>().ForEach([&](ECS::TMut<SFoliageComponent> FoliageMut)
        {
            SFoliageComponent& Foliage = FoliageMut.GetMut();
            if (Scene != nullptr && Foliage.CollisionGroupID != 0)
            {
                Scene->DestroyStaticBodyGroup(Foliage.CollisionGroupID);
            }
            Foliage.CollisionGroupID = 0;
            Foliage.CollisionBakedVersion = 0;
        });
    }
}
