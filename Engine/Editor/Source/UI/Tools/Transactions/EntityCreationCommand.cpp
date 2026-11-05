#include "EntityCreationCommand.h"
#include "World/ECS/Registry.h"

#include <algorithm>

#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EditorComponent.h"

namespace Lumina
{
    FEntityCreationCommand::FEntityCreationCommand(CWorld* InWorld)
        : World(InWorld)
    {
        CaptureLiveEntities(LiveBefore);
    }

    void FEntityCreationCommand::CaptureLiveEntities(TVector<ECS::FEntity>& Out) const
    {
        Out.Reset();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        // Excluded for the same reason the registry snapshot excludes them, they are not the document.
        Out.Reserve(Registry.NumEntities());
        Registry.ForEachEntityExcept<FEditorComponent>([&](ECS::FEntity E) { Out.Add(E); });

        // Sorted so Finalize's diff is a binary search, and handles only with no component data touched.
        Algo::Sort(Out);
    }

    void FEntityCreationCommand::Finalize()
    {
        Created.Reset();
        CreatedData.Reset();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        TVector<ECS::FEntity> LiveAfter;
        CaptureLiveEntities(LiveAfter);

        for (ECS::FEntity E : LiveAfter)
        {
            if (!Algo::BinarySearch(LiveBefore, E))
            {
                Created.Add(E);
            }
        }

        // Any shortfall means entities went away, and this command has no before-image to restore them.
        const int64 DestroyedCount = (int64)LiveBefore.Num() + (int64)Created.Num() - (int64)LiveAfter.Num();
        if (DestroyedCount != 0)
        {
            LOG_ERROR("FEntityCreationCommand: {} entities were destroyed inside a creation-only "
                      "transaction; undo cannot restore them. Use BeginTransaction for this operation.",
                      DestroyedCount);
        }

        if (Created.IsEmpty())
        {
            return;
        }

        // SerializeEntity records each parent and sibling index, so links inside and outside the created set restore.
        FMemoryWriter Writer(CreatedData);
        FObjectProxyArchiver Ar(Writer, false);

        int32 NumCreated = Created.Num();
        Ar << NumCreated;

        for (ECS::FEntity E : Created)
        {
            ECS::FEntity Mutable = E;
            ECS::Utils::SerializeEntity(Ar, Registry, Mutable);
        }
    }

    void FEntityCreationCommand::Undo()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

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
        if (W == nullptr || CreatedData.IsEmpty())
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        FMemoryReader Reader(CreatedData);
        FObjectProxyArchiver Ar(Reader, true);

        int32 NumCreated = 0;
        Ar << NumCreated;

        ECS::Utils::FParentLinks Links;
        for (int32 i = 0; i < NumCreated; ++i)
        {
            ECS::FEntity Entity = ECS::NullEntity;
            ECS::Utils::SerializeEntity(Ar, Registry, Entity, &Links);
        }
        Links.Apply(Registry);

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }
}
