#include "EntityDestroyCommand.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina
{
    void FEntityDestroyCommand::CollectCandidates(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Roots,
                                                  TVector<ECS::FEntity>& Out)
    {
        for (ECS::FEntity Root : Roots)
        {
            if (!Registry.IsValid(Root))
            {
                continue;
            }

            // Ancestors first, so the reversed redo destroys every child before its parent.
            Out.AddUnique(Root);

            Registry.GetHierarchy().ForEachDescendant(Root, [&](ECS::FEntity Descendant)
            {
                Out.AddUnique(Descendant);
            });
        }
    }

    FEntityDestroyCommand::FEntityDestroyCommand(CWorld* InWorld, const TVector<ECS::FEntity>& InCandidates)
        : World(InWorld)
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        FMemoryWriter Writer(Data);
        FObjectProxyArchiver Ar(Writer, false);

        for (ECS::FEntity Candidate : InCandidates)
        {
            if (!Registry.IsValid(Candidate))
            {
                continue;
            }

            FEntry Entry;
            Entry.Entity = Candidate;
            Entry.Offset = Ar.Tell();

            ECS::FEntity Mutable = Candidate;
            ECS::Utils::SerializeEntity(Ar, Registry, Mutable);

            Entry.Size = Ar.Tell() - Entry.Offset;
            Entries.Add(Entry);
        }
    }

    void FEntityDestroyCommand::Finalize()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            Entries.Reset();
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        // A candidate still alive was never part of the delete, so it owes no undo step.
        TVector<FEntry> Kept;
        Kept.Reserve(Entries.Num());
        for (const FEntry& Entry : Entries)
        {
            if (!Registry.IsValid(Entry.Entity))
            {
                Kept.Add(Entry);
            }
        }

        Entries = Move(Kept);
    }

    void FEntityDestroyCommand::Undo()
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || Entries.IsEmpty())
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        FMemoryReader Reader(Data);
        FObjectProxyArchiver Ar(Reader, true);

        ECS::Utils::FParentLinks Links;
        for (const FEntry& Entry : Entries)
        {
            Ar.Seek(Entry.Offset);

            ECS::FEntity Restored = ECS::NullEntity;
            ECS::Utils::SerializeEntity(Ar, Registry, Restored, &Links);
        }
        Links.Apply(Registry);

        // A restored node carries the prefab data it had when deleted, which a later prefab update may have moved past.
        for (const FEntry& Entry : Entries)
        {
            CPrefab::MarkInstanceStale(Registry, Entry.Entity);
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityDestroyCommand::Redo()
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();
        if (W == nullptr || Entries.IsEmpty())
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        // Reverse, so each child goes before the parent that would otherwise take it along.
        for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
        {
            if (Registry.IsValid(It->Entity))
            {
                Registry.Destroy(It->Entity);
            }
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }
}
