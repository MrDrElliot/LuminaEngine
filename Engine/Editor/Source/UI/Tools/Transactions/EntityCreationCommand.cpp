#include "EntityCreationCommand.h"

#include <algorithm>

#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"

namespace Lumina
{
    FEntityCreationCommand::FEntityCreationCommand(CWorld* InWorld)
        : World(InWorld)
    {
        CaptureLiveEntities(LiveBefore);
    }

    void FEntityCreationCommand::CaptureLiveEntities(TVector<entt::entity>& Out) const
    {
        Out.clear();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Editor-only entities are excluded for the same reason the registry snapshot excludes them:
        // they are not part of the document, and they come and go on their own.
        auto View = Registry.view<entt::entity>(entt::exclude<FEditorComponent>);
        Out.reserve(View.size_hint());
        View.each([&](entt::entity E) { Out.push_back(E); });

        // Sorted so Finalize's diff is a binary search rather than a nested scan. Handles only -- no
        // component data is touched here, which is the whole point.
        std::sort(Out.begin(), Out.end());
    }

    void FEntityCreationCommand::Finalize()
    {
        Created.clear();
        CreatedData.clear();
        ExternalParents.clear();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        TVector<entt::entity> LiveAfter;
        CaptureLiveEntities(LiveAfter);

        for (entt::entity E : LiveAfter)
        {
            if (!std::binary_search(LiveBefore.begin(), LiveBefore.end(), E))
            {
                Created.push_back(E);
            }
        }

        // Misuse guard. With nothing destroyed, After == Before + Created exactly; any shortfall is
        // entities that went away. This command has no before-image, so Undo cannot bring them back --
        // and silently committing a step that half-restores the world is far worse than saying so.
        const int64 DestroyedCount = (int64)LiveBefore.size() + (int64)Created.size() - (int64)LiveAfter.size();
        if (DestroyedCount != 0)
        {
            LOG_ERROR("FEntityCreationCommand: {} entities were destroyed inside a creation-only "
                      "transaction; undo cannot restore them. Use BeginTransaction for this operation.",
                      DestroyedCount);
        }

        if (Created.empty())
        {
            return;
        }

        // Serialize only what was added. SerializeEntity records each entity's own relationship
        // component, and handles round-trip exactly, so links WITHIN the created set restore themselves.
        FMemoryWriter Writer(CreatedData);
        FObjectProxyArchiver Ar(Writer, false);

        int32 NumCreated = (int32)Created.size();
        Ar << NumCreated;

        for (entt::entity E : Created)
        {
            entt::entity Mutable = E;
            ECS::Utils::SerializeEntity(Ar, Registry, Mutable);

            // A parent outside the created set is the one link the images cannot carry.
            if (const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(E))
            {
                if (Rel->Parent != entt::null
                    && !std::binary_search(Created.begin(), Created.end(), Rel->Parent))
                {
                    ExternalParents.push_back({ E, Rel->Parent });
                }
            }
        }
    }

    void FEntityCreationCommand::Undo()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        // DestroyEntityHierarchy detaches from the parent's child list before destroying, which is
        // exactly the fixup the created entities' own images cannot do. Descendants inside the created
        // set are taken out by their root, so the validity guard is what makes the second visit a no-op.
        for (entt::entity E : Created)
        {
            if (Registry.valid(E))
            {
                ECS::Utils::DestroyEntityHierarchy(Registry, E);
            }
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityCreationCommand::Redo()
    {
        CWorld* W = World.Get();
        if (W == nullptr || CreatedData.empty())
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryReader Reader(CreatedData);
        FObjectProxyArchiver Ar(Reader, true);

        int32 NumCreated = 0;
        Ar << NumCreated;

        for (int32 i = 0; i < NumCreated; ++i)
        {
            entt::entity Entity = entt::null;
            ECS::Utils::SerializeEntity(Ar, Registry, Entity);
        }

        // Re-attach to pre-existing parents. bPreserveWorld = false to match how DuplicateEntity parented
        // them originally -- preserving world here would recompute the local transform off the restored
        // one and drift the entity every redo.
        for (const FExternalParent& Link : ExternalParents)
        {
            if (Registry.valid(Link.Child) && Registry.valid(Link.Parent))
            {
                ECS::Utils::ReparentEntity(Registry, Link.Child, Link.Parent, false);
            }
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }
}
