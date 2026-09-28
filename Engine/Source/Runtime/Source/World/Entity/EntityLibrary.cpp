#include "RuntimePCH.h"

#include "EntityLibrary.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/EntityUtils.h"

#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        const FRelationshipComponent* RelationshipOf(CWorld* World, ECS::FEntity Entity)
        {
            return World != nullptr ? World->TryGetComponent<FRelationshipComponent>(Entity) : nullptr;
        }
    }

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
        const FRelationshipComponent* Relationship = RelationshipOf(World, Entity);
        return Relationship != nullptr ? Relationship->First : ECS::NullEntity;
    }

    ECS::FEntity CEntityLibrary::GetNextSibling(CWorld* World, ECS::FEntity Entity)
    {
        const FRelationshipComponent* Relationship = RelationshipOf(World, Entity);
        return Relationship != nullptr ? Relationship->Next : ECS::NullEntity;
    }

    void CEntityLibrary::GetAncestorChain(CWorld* World, ECS::FEntity Entity, TVector<ECS::FEntity>& Out)
    {
        if (World == nullptr)
        {
            return;
        }

        for (ECS::FEntity Current = Entity; Current != ECS::NullEntity; )
        {
            Out.push_back(Current);
            const FRelationshipComponent* Relationship = RelationshipOf(World, Current);
            Current = Relationship != nullptr ? Relationship->Parent : ECS::NullEntity;
        }
    }

    FVector3 CEntityLibrary::GetRenderLocation(CWorld* World, ECS::FEntity Entity)
    {
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return FVector3(0.0f);
        }

        return ECS::Utils::GetRenderLocation(ECS::GetWorldRegistry(*World), Entity);
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

        TVector<ECS::FEntity> Stack;
        Stack.push_back(Entity);
        while (!Stack.empty())
        {
            const ECS::FEntity Node = Stack.back();
            Stack.pop_back();
            Out.push_back(Node);

            const FRelationshipComponent* Relationship = RelationshipOf(World, Node);
            for (ECS::FEntity Child = Relationship != nullptr ? Relationship->First : ECS::NullEntity;
                 Child != ECS::NullEntity; )
            {
                Stack.push_back(Child);
                const FRelationshipComponent* ChildRelationship = RelationshipOf(World, Child);
                Child = ChildRelationship != nullptr ? ChildRelationship->Next : ECS::NullEntity;
            }
        }
    }
}
