#include "RuntimePCH.h"

#include "EntityLibrary.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/EntityUtils.h"

#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    void CEntityLibrary::SetTag(CWorld* World, ECS::FEntity Entity, const FName& Tag)
    {
        if (World != nullptr)
        {
            ECS::Utils::SetEntityTag(ECS::GetWorldRegistry(*World), Entity, Tag);
        }
    }

    FName CEntityLibrary::GetTag(CWorld* World, ECS::FEntity Entity)
    {
        if (World == nullptr)
        {
            return NAME_None;
        }

        const STagComponent* Component = World->TryGetComponent<STagComponent>(Entity);
        return Component != nullptr ? Component->Tag : NAME_None;
    }

    ECS::FEntity CEntityLibrary::GetFirstChild(CWorld* World, ECS::FEntity Entity)
    {
        return World != nullptr ? World->GetHierarchy().GetFirstChild(Entity) : ECS::NullEntity;
    }

    ECS::FEntity CEntityLibrary::GetNextSibling(CWorld* World, ECS::FEntity Entity)
    {
        return World != nullptr ? World->GetHierarchy().GetNextSibling(Entity) : ECS::NullEntity;
    }

    void CEntityLibrary::GetAncestorChain(CWorld* World, ECS::FEntity Entity, TVector<ECS::FEntity>& Out)
    {
        if (World == nullptr)
        {
            return;
        }

        const ECS::FHierarchy& Hierarchy = World->GetHierarchy();
        for (ECS::FEntity Current = Entity; Current != ECS::NullEntity; Current = Hierarchy.GetParent(Current))
        {
            Out.push_back(Current);
        }
    }

    void CEntityLibrary::SetLocalTransforms(CWorld* World, const ECS::FEntity* Entities, int32 EntityCount, const FTransform* Transforms, int32 TransformCount)
    {
        if (World == nullptr || Entities == nullptr || Transforms == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const int32 Count = Math::Min(EntityCount, TransformCount);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entities[Index]))
            {
                Transform->SetLocalTransform(Transforms[Index]);
            }
        }
    }

    void CEntityLibrary::GetSubtree(CWorld* World, ECS::FEntity Entity, TVector<ECS::FEntity>& Out)
    {
        if (World == nullptr)
        {
            return;
        }

        Out.push_back(Entity);
        World->GetHierarchy().ForEachDescendant(Entity, [&Out](ECS::FEntity Descendant) { Out.push_back(Descendant); });
    }
}
