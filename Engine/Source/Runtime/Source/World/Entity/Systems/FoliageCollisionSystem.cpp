#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "FoliageCollisionSystem.h"
#include "SystemContext.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Memory/Allocators/Allocator.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/FoliageComponent.h"

namespace Lumina
{
    FSystemAccess SFoliageCollisionSystem::Access = FSystemAccess{}
        .Write<SFoliageComponent>()
        .Write<SystemResource::PhysicsQuery>();

    void SFoliageCollisionSystem::Update(const FSystemContext& Context) noexcept
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

            // Scratch-stack backed and consumed inside this call; no heap traffic per rebake.
            FMemMark Mark;
            TScratchVector<Physics::FStaticInstanceDesc> Descs;
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

                const CCollisionShape* Shape = Type.CollisionShape.Get();
                if (Shape != nullptr && !Shape->HasCollision())
                {
                    Shape = nullptr;
                }

                const CStaticMesh* Mesh = Type.Mesh.Get();
                if (Shape == nullptr)
                {
                    if (Mesh == nullptr || Mesh->HasAnyFlag(OF_NeedsLoad) || Mesh->GetMeshResource().MeshletData.IsEmpty())
                    {
                        bSourcesReady = false;
                        continue;
                    }
                }

                Physics::FStaticInstanceDesc& Desc = Descs.emplace_back();
                Desc.Position = Inst.Position;
                Desc.Rotation = Inst.GetRotationQuat();
                Desc.Scale    = Inst.Scale;
                Desc.Shape    = Shape;
                Desc.Mesh     = Shape == nullptr ? Mesh : nullptr;
                Desc.Material = Type.PhysicsMaterial.Get();
                Desc.bConvex  = Type.bConvexCollision;
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

    void SFoliageCollisionSystem::Teardown(const FSystemContext& Context) noexcept
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
