#include "EntityDestroyCommand.h"
#include "World/ECS/Registry.h"

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

            // Ancestors first, so a restore recreates a parent before the child that points at it.
            Out.AddUnique(Root);

            TVector<ECS::FEntity> Descendants;
            ECS::Utils::CollectDescendants(Registry, Root, Descendants);
            for (ECS::FEntity Descendant : Descendants)
            {
                if (Registry.IsValid(Descendant))
                {
                    Out.AddUnique(Descendant);
                }
            }
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

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
            Entries.push_back(Entry);
        }
    }

    void FEntityDestroyCommand::Finalize()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            Entries.clear();
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // A candidate still alive was never part of the delete, so it owes no undo step.
        TVector<FEntry> Kept;
        Kept.reserve(Entries.size());
        for (const FEntry& Entry : Entries)
        {
            if (!Registry.IsValid(Entry.Entity))
            {
                Kept.push_back(Entry);
            }
        }

        Entries = Move(Kept);
    }

    void FEntityDestroyCommand::Undo()
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || Entries.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryReader Reader(Data);
        FObjectProxyArchiver Ar(Reader, true);

        for (const FEntry& Entry : Entries)
        {
            Ar.Seek(Entry.Offset);

            ECS::FEntity Restored = ECS::NullEntity;
            ECS::Utils::SerializeEntity(Ar, Registry, Restored);
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
        if (W == nullptr || Entries.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Reverse, so a child goes before the parent whose link list would otherwise still name it.
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
