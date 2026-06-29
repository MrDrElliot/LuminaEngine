#include "EditorAssetDropHandlers.h"

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    FEditorAssetDropRegistry& FEditorAssetDropRegistry::Get()
    {
        static FEditorAssetDropRegistry Instance;
        static bool bRegistered = false;
        if (!bRegistered)
        {
            bRegistered = true;

            // Static mesh: spawn an entity carrying the mesh; if dropped on an existing
            // mesh entity, just replace its mesh asset.
            Instance.Register(CStaticMesh::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget) -> entt::entity
                {
                    CStaticMesh* Mesh = Cast<CStaticMesh>(Asset);
                    if (Mesh == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        if (SStaticMeshComponent* Existing = Registry.try_get<SStaticMeshComponent>(DropTarget))
                        {
                            Existing->SetStaticMesh(Mesh);
                            return DropTarget;
                        }
                    }

                    entt::entity Entity = World->ConstructEntity(Mesh->GetName(), SpawnTransform);
                    SStaticMeshComponent& MeshComponent = Registry.emplace<SStaticMeshComponent>(Entity);
                    MeshComponent.SetStaticMesh(Mesh);

                    if (DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
                    }

                    return Entity;
                });

            // Material: only meaningful when dropped on an existing mesh entity. Sets material slot 0.
            Instance.Register(CMaterialInterface::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& /*SpawnTransform*/, entt::entity DropTarget) -> entt::entity
                {
                    CMaterialInterface* Material = Cast<CMaterialInterface>(Asset);
                    if (Material == nullptr || World == nullptr || DropTarget == entt::null)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (!Registry.valid(DropTarget))
                    {
                        return entt::null;
                    }

                    SStaticMeshComponent* MeshComponent = Registry.try_get<SStaticMeshComponent>(DropTarget);
                    if (MeshComponent == nullptr)
                    {
                        return entt::null;
                    }

                    if (MeshComponent->MaterialOverrides.empty())
                    {
                        MeshComponent->MaterialOverrides.resize(1);
                    }
                    MeshComponent->MaterialOverrides[0] = Material;
                    return DropTarget;
                });

            // Prefab: instantiate at SpawnTransform under DropTarget (or as a root).
            Instance.Register(CPrefab::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget) -> entt::entity
                {
                    CPrefab* Prefab = Cast<CPrefab>(Asset);
                    if (Prefab == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }
                    return Prefab->Instantiate(World, SpawnTransform, DropTarget);
                });
        }
        return Instance;
    }

    void FEditorAssetDropRegistry::Register(FName AssetClass, FEditorAssetDropHandler Handler)
    {
        Handlers[AssetClass] = eastl::move(Handler);
    }

    const FEditorAssetDropHandler* FEditorAssetDropRegistry::FindHandler(FName AssetClass) const
    {
        // Direct hit on the concrete class name first.
        auto It = Handlers.find(AssetClass);
        if (It != Handlers.end())
        {
            return &It->second;
        }

        // Walk up the reflected class chain so a registration on CMaterialInterface picks
        // up CMaterial / CMaterialInstance assets without a separate entry per subclass.
        CClass* Class = FindObject<CClass>(AssetClass);
        while (Class != nullptr)
        {
            CClass* Super = Class->GetSuperClass();
            if (Super == nullptr)
            {
                break;
            }
            It = Handlers.find(Super->GetName());
            if (It != Handlers.end())
            {
                return &It->second;
            }
            Class = Super;
        }

        return nullptr;
    }
}
