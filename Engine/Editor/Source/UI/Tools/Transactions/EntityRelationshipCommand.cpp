#include "EntityRelationshipCommand.h"
#include "World/ECS/Registry.h"

#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    void FEntityRelationshipCommand::CollectAffected(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Seeds,
                                                     ECS::FEntity NewParent, TVector<ECS::FEntity>& Out)
    {
        auto AddWithChildren = [&](ECS::FEntity Entity)
        {
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }

            Out.AddUnique(Entity);
            Registry.GetHierarchy().ForEachChild(Entity, [&](ECS::FEntity Child) { Out.AddUnique(Child); });
        };

        for (ECS::FEntity Seed : Seeds)
        {
            if (!Registry.IsValid(Seed))
            {
                continue;
            }

            AddWithChildren(Seed);

            // The old parent's whole child list, since moving one child shifts the sibling index of the rest.
            AddWithChildren(Registry.GetHierarchy().GetParent(Seed));
        }

        AddWithChildren(NewParent);
    }

    FEntityRelationshipCommand::FEntityRelationshipCommand(CWorld* InWorld, TVector<ECS::FEntity> InEntities)
        : World(InWorld)
        , Entities(Move(InEntities))
    {
        Capture(Before);
    }

    void FEntityRelationshipCommand::Finalize()
    {
        Capture(After);
    }

    bool FEntityRelationshipCommand::IsNoOp() const
    {
        return Before == After;
    }

    void FEntityRelationshipCommand::Capture(TVector<FRecord>& Out) const
    {
        LUMINA_PROFILE_SCOPE();

        // Index-aligned even if an entity dies mid-edit, which records no parent and is skipped.
        Out.assign(Entities.size(), FRecord());

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const ECS::FEntity Entity = Entities[i];
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            Out[i].Parent = Hierarchy.GetParent(Entity);
            Out[i].SiblingIndex = Out[i].Parent.IsNull() ? 0u : Hierarchy.GetSiblingIndex(Entity);
        }
    }

    void FEntityRelationshipCommand::Apply(const TVector<FRecord>& In) const
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || In.size() != Entities.size())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Every sibling of each affected parent is in the set, so detaching them all and reattaching by index reproduces the image.
        ECS::Utils::FParentLinks Links;
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const ECS::FEntity Entity = Entities[i];
            if (!Registry.IsValid(Entity))
            {
                continue;   // destroyed since the edit; its own transaction owns bringing it back
            }

            Registry.DetachFromParent(Entity);
            if (!In[i].Parent.IsNull())
            {
                Links.Links.push_back({ Entity, In[i].Parent, In[i].SiblingIndex });
            }

            // The parent chain moved, so every restored entity owes a world-matrix recompute.
            if (Registry.HasAll<STransformComponent>(Entity))
            {
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            }
        }
        Links.Apply(Registry);

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityRelationshipCommand::Undo() { Apply(Before); }
    void FEntityRelationshipCommand::Redo() { Apply(After); }
}
