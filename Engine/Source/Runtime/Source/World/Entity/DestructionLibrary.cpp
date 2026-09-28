#include "RuntimePCH.h"
#include "DestructionLibrary.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Components/DestructibleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "World/World.h"

namespace Lumina
{
    bool CDestructionLibrary::FractureEntity(CWorld* World, ECS::FEntity Entity, FVector3 Origin, float Strength)
    {
        return World != nullptr && World->FractureEntity(Entity, Origin, Strength);
    }

    bool CDestructionLibrary::ShatterMesh(CWorld* World, CStaticMesh* Mesh, FTransform Transform, FVector3 Origin, float Strength,
                                          int32 Pieces, float Lifetime)
    {
        if (World == nullptr || Mesh == nullptr)
        {
            return false;
        }

        const ECS::FEntity Entity = World->ConstructEntity("Shattered", Transform);
        SStaticMeshComponent& MeshComponent = World->EmplaceComponent<SStaticMeshComponent>(Entity);
        MeshComponent.StaticMesh = Mesh;

        SDestructibleComponent& Destructible = World->EmplaceComponent<SDestructibleComponent>(Entity);
        Destructible.FragmentCount = Math::Max(Pieces, 2);
        Destructible.FragmentLifetime = Math::Max(Lifetime, 0.1f);
        Destructible.bDestroyOriginal = true;

        if (!World->FractureEntity(Entity, Origin, Strength))
        {
            World->DestroyEntity(Entity);
            return false;
        }
        return true;
    }
}
