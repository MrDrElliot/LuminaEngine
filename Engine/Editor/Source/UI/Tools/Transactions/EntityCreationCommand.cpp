#include "EntityCreationCommand.h"
#include "World/ECS/Registry.h"

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

    void FEntityCreationCommand::CaptureLiveEntities(TVector<ECS::FEntity>& Out) const
    {
        Out.clear();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Excluded for the same reason the registry snapshot excludes them, they are not the document.
        Out.reserve(Registry.NumEntities());
        Registry.ForEachEntityExcept<FEditorComponent>([&](ECS::FEntity E) { Out.push_back(E); });

        // Sorted so Finalize's diff is a binary search, and handles only with no component data touched.
        Algo::Sort(Out);
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        TVector<ECS::FEntity> LiveAfter;
        CaptureLiveEntities(LiveAfter);

        for (ECS::FEntity E : LiveAfter)
        {
            if (!Algo::BinarySearch(LiveBefore, E))
            {
                Created.push_back(E);
            }
        }

        // Any shortfall means entities went away, and this command has no before-image to restore them.
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

        // SerializeEntity records each relationship component, so links within the created set restore.
        FMemoryWriter Writer(CreatedData);
        FObjectProxyArchiver Ar(Writer, false);

        int32 NumCreated = (int32)Created.size();
        Ar << NumCreated;

        for (ECS::FEntity E : Created)
        {
            ECS::FEntity Mutable = E;
            ECS::Utils::SerializeEntity(Ar, Registry, Mutable);

            // A parent outside the created set is the one link the images cannot carry.
            if (const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(E))
            {
                if (Rel->Parent != ECS::NullEntity
                    && !Algo::BinarySearch(Created, Rel->Parent))
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Descendants inside the created set are taken by their root, so the validity guard makes it a no-op.
        for (ECS::FEntity E : Created)
        {
            if (Registry.IsValid(E))
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryReader Reader(CreatedData);
        FObjectProxyArchiver Ar(Reader, true);

        int32 NumCreated = 0;
        Ar << NumCreated;

        for (int32 i = 0; i < NumCreated; ++i)
        {
            ECS::FEntity Entity = ECS::NullEntity;
            ECS::Utils::SerializeEntity(Ar, Registry, Entity);
        }

        // Preserving world here would recompute the local transform and drift the entity every redo.
        for (const FExternalParent& Link : ExternalParents)
        {
            if (Registry.IsValid(Link.Child) && Registry.IsValid(Link.Parent))
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
