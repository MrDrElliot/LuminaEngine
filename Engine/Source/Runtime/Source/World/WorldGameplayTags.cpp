#include "RuntimePCH.h"

#include "HierarchicalTags/HierarchicalTagComponent.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        SHierarchicalTag TagFromId(uint32 TagId)
        {
            SHierarchicalTag Tag;
            Tag.TagName = FHierarchicalTagRegistry::Get().GetTagName(TagId);
            return Tag;
        }

        const SHierarchicalTagComponent* ComponentOf(CWorld& World, ECS::FEntity Entity, uint32 TagId)
        {
            if (TagId == 0)
            {
                return nullptr;
            }
            return World.TryGetComponent<SHierarchicalTagComponent>(Entity);
        }
    }
    void CWorld::GetGameplayTags(ECS::FEntity Entity, TVector<uint32>& Out)
    {
        const SHierarchicalTagComponent* Tags = TryGetComponent<SHierarchicalTagComponent>(Entity);
        if (Tags == nullptr)
        {
            return;
        }

        for (const SHierarchicalTag& Owned : Tags->Tags.Tags)
        {
            Out.Add(Owned.GetId());
        }
    }

    void CWorld::AddGameplayTag(ECS::FEntity Entity, uint32 TagId)
    {
        if (TagId == 0)
        {
            return;
        }
        const SHierarchicalTag Tag = TagFromId(TagId);
        if (Tag.IsValid())
        {
            GetOrEmplaceComponent<SHierarchicalTagComponent>(Entity).Tags.AddTag(Tag);
        }
    }

    void CWorld::RemoveGameplayTag(ECS::FEntity Entity, uint32 TagId)
    {
        if (TagId == 0)
        {
            return;
        }
        if (SHierarchicalTagComponent* Comp = TryGetComponent<SHierarchicalTagComponent>(Entity))
        {
            Comp->Tags.RemoveTag(TagFromId(TagId));
        }
    }

    bool CWorld::HasGameplayTag(ECS::FEntity Entity, uint32 TagId)
    {
        const SHierarchicalTagComponent* Comp = ComponentOf(*this, Entity, TagId);
        if (Comp == nullptr)
        {
            return false;
        }
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        for (const SHierarchicalTag& Owned : Comp->Tags.Tags)
        {
            if (Registry.Matches(Owned.GetId(), TagId))
            {
                return true;
            }
        }
        return false;
    }

    bool CWorld::HasGameplayTagExact(ECS::FEntity Entity, uint32 TagId)
    {
        const SHierarchicalTagComponent* Comp = ComponentOf(*this, Entity, TagId);
        if (Comp == nullptr)
        {
            return false;
        }
        for (const SHierarchicalTag& Owned : Comp->Tags.Tags)
        {
            if (Owned.GetId() == TagId)
            {
                return true;
            }
        }
        return false;
    }

    void CWorld::ClearGameplayTags(ECS::FEntity Entity)
    {
        if (SHierarchicalTagComponent* Comp = TryGetComponent<SHierarchicalTagComponent>(Entity))
        {
            Comp->Tags.Tags.Reset();
        }
    }
}
