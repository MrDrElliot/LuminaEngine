#include "EntityDestroyCommand.h"

#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina
{
    void FEntityDestroyCommand::CollectCandidates(FEntityRegistry& Registry, const TVector<entt::entity>& Roots,
                                                  TVector<entt::entity>& Out)
    {
        for (entt::entity Root : Roots)
        {
            if (!Registry.valid(Root))
            {
                continue;
            }

            // Ancestors first, so a restore recreates a parent before the child that points at it.
            Out.AddUnique(Root);

            TVector<entt::entity> Descendants;
            ECS::Utils::CollectDescendants(Registry, Root, Descendants);
            for (entt::entity Descendant : Descendants)
            {
                if (Registry.valid(Descendant))
                {
                    Out.AddUnique(Descendant);
                }
            }
        }
    }

    FEntityDestroyCommand::FEntityDestroyCommand(CWorld* InWorld, const TVector<entt::entity>& InCandidates)
        : World(InWorld)
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryWriter Writer(Data);
        FObjectProxyArchiver Ar(Writer, false);

        for (entt::entity Candidate : InCandidates)
        {
            if (!Registry.valid(Candidate))
            {
                continue;
            }

            FEntry Entry;
            Entry.Entity = Candidate;
            Entry.Offset = Ar.Tell();

            entt::entity Mutable = Candidate;
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        // A candidate still alive was never part of the delete, so it owes no undo step.
        TVector<FEntry> Kept;
        Kept.reserve(Entries.size());
        for (const FEntry& Entry : Entries)
        {
            if (!Registry.valid(Entry.Entity))
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryReader Reader(Data);
        FObjectProxyArchiver Ar(Reader, true);

        for (const FEntry& Entry : Entries)
        {
            Ar.Seek(Entry.Offset);

            entt::entity Restored = entt::null;
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Reverse, so a child goes before the parent whose link list would otherwise still name it.
        for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
        {
            if (Registry.valid(It->Entity))
            {
                Registry.destroy(It->Entity);
            }
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }
}
