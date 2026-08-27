#include "EntityRelationshipCommand.h"

#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        bool SameLink(const FRelationshipComponent& A, const FRelationshipComponent& B)
        {
            return A.Children == B.Children
                && A.First    == B.First
                && A.Prev     == B.Prev
                && A.Next     == B.Next
                && A.Parent   == B.Parent;
        }

    }

    void FEntityRelationshipCommand::CollectAffected(FEntityRegistry& Registry, const TVector<entt::entity>& Seeds,
                                                     entt::entity NewParent, TVector<entt::entity>& Out)
    {
        auto AddWithChildren = [&](entt::entity Entity)
        {
            if (Entity == entt::null || !Registry.valid(Entity))
            {
                return;
            }

            Out.AddUnique(Entity);
            ECS::Utils::ForEachChild(Registry, Entity, [&](entt::entity Child) { Out.AddUnique(Child); });
        };

        for (entt::entity Seed : Seeds)
        {
            if (!Registry.valid(Seed))
            {
                continue;
            }

            AddWithChildren(Seed);

            // The old parent's whole child list, since unlinking rewrites First and the Prev/Next neighbors.
            if (const FRelationshipComponent* Link = Registry.try_get<FRelationshipComponent>(Seed))
            {
                AddWithChildren(Link->Parent);
            }
        }

        AddWithChildren(NewParent);
    }

    FEntityRelationshipCommand::FEntityRelationshipCommand(CWorld* InWorld, TVector<entt::entity> InEntities)
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
        if (Before.size() != After.size())
        {
            return false;
        }

        for (SIZE_T i = 0; i < Before.size(); ++i)
        {
            if (Before[i].bPresent != After[i].bPresent)
            {
                return false;
            }

            if (Before[i].bPresent && !SameLink(Before[i].Link, After[i].Link))
            {
                return false;
            }
        }

        return true;
    }

    void FEntityRelationshipCommand::Capture(TVector<FRecord>& Out) const
    {
        LUMINA_PROFILE_SCOPE();

        // Index-aligned even if an entity dies mid-edit, which records an absent link and is skipped.
        Out.assign(Entities.size(), FRecord());

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const entt::entity Entity = Entities[i];
            if (!Registry.valid(Entity))
            {
                continue;
            }

            if (const FRelationshipComponent* Link = Registry.try_get<FRelationshipComponent>(Entity))
            {
                Out[i].bPresent = true;
                Out[i].Link = *Link;
            }
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const entt::entity Entity = Entities[i];
            if (!Registry.valid(Entity))
            {
                continue;   // destroyed since the edit; its own transaction owns bringing it back
            }

            if (In[i].bPresent)
            {
                Registry.emplace_or_replace<FRelationshipComponent>(Entity, In[i].Link);
            }
            else if (Registry.all_of<FRelationshipComponent>(Entity))
            {
                Registry.remove<FRelationshipComponent>(Entity);
            }

            // The parent chain moved, so every restored entity owes a world-matrix recompute.
            if (Registry.all_of<STransformComponent>(Entity))
            {
                Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
            }
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityRelationshipCommand::Undo() { Apply(Before); }
    void FEntityRelationshipCommand::Redo() { Apply(After); }
}
