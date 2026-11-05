#pragma once

#include "World/ECS/Registry.h"



#include "ModuleAPI.h"
#include "Containers/Algorithm.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "SystemResources.h"

namespace Lumina
{
    // Components an access is limited to holding and lacking, so two systems that can never touch the same entity run together.
    struct FAccessMembership
    {
        TVector<uint32> With;
        TVector<uint32> Without;

        NODISCARD static bool AreDisjoint(const FAccessMembership& A, const FAccessMembership& B)
        {
            return Algo::AnyOf(A.With, [&B](uint32 Id) { return Algo::Contains(B.Without, Id); })
                || Algo::AnyOf(A.Without, [&B](uint32 Id) { return Algo::Contains(B.With, Id); });
        }
    };

    // One component access and the membership it goes through; Membership is kIndexNone when it may reach any entity.
    struct FComponentAccess
    {
        uint32 Id         = 0;
        int32  Membership = Constants::kIndexNone;
        bool   bWrite     = false;

        NODISCARD bool operator==(const FComponentAccess&) const = default;
    };

    // What a system reads and writes, so the scheduler runs non-conflicting systems together; a CExclusiveSystem conflicts with all.
    struct FSystemAccess
    {
        TVector<uint32> Writes;
        TVector<uint32> Reads;

        // The declared component pools, created before a parallel segment since creating one resizes the shared pool table.
        TVector<ECS::FComponentTypeID> Pools;
        // Pools a Changed or Added query filters on, so their change tracking is on before the system goes wide.
        TVector<ECS::FComponentTypeID> TrackedPools;

        // Event types it sends, which reach a reader only once the sender's segment flushes.
        TVector<uint32> SentEvents;

        // Every component access with the entities it is limited to, so filters that exclude each other do not conflict.
        TVector<FComponentAccess>  ComponentAccesses;
        TVector<FAccessMembership> Memberships;

        bool            bExclusive = false;

        template<typename... Ts>
        FSystemAccess& Write()
        {
            (Writes.AddGetRef(static_cast<uint32>(ECS::GetComponentTypeID<Ts>())), ...);
            (AddUnfiltered<Ts>(true), ...);
            (AddPoolAssurer<Ts>(), ...);
            return *this;
        }

        template<typename... Ts>
        FSystemAccess& Read()
        {
            (Reads.AddGetRef(static_cast<uint32>(ECS::GetComponentTypeID<Ts>())), ...);
            (AddUnfiltered<Ts>(false), ...);
            (AddPoolAssurer<Ts>(), ...);
            return *this;
        }

        template<typename T>
        void AddUnfiltered(bool bWrite)
        {
            if constexpr (!ECS::CResource<T>)
            {
                ComponentAccesses.Add(FComponentAccess{ static_cast<uint32>(ECS::GetComponentTypeID<T>()), Constants::kIndexNone, bWrite });
            }
        }

        template<typename T>
        void AddPoolAssurer()
        {
            if constexpr (!ECS::CResource<T>)
            {
                Pools.Add(ECS::GetComponentTypeID<T>());
            }
        }

        static FSystemAccess Exclusive()
        {
            FSystemAccess A;
            A.bExclusive = true;
            return A;
        }

        // Sets are tiny (a handful of types), so a nested scan beats hashing.
        static bool Intersects(const TVector<uint32>& X, const TVector<uint32>& Y)
        {
            return Algo::AnyOf(X, [&Y](uint32 A) { return Algo::Contains(Y, A); });
        }

        // Ids that overlap with a write conflict unless every pair of component accesses on them is limited to disjoint entities.
        static bool Conflicts(const FSystemAccess& A, const FSystemAccess& B)
        {
            if (A.bExclusive || B.bExclusive)
            {
                return true;
            }
            uint32 Unused = 0;
            return FindConflictingId(A, B, Unused);
        }

        // The first component, resource or event id that makes two shared systems conflict; ignores bExclusive.
        static bool FindConflictingId(const FSystemAccess& A, const FSystemAccess& B, uint32& OutId)
        {
            for (const uint32 Id : A.Writes)
            {
                if ((Algo::Contains(B.Writes, Id) || Algo::Contains(B.Reads, Id)) && Overlaps(A, B, Id))
                {
                    OutId = Id;
                    return true;
                }
            }
            for (const uint32 Id : A.Reads)
            {
                if (Algo::Contains(B.Writes, Id) && Overlaps(A, B, Id))
                {
                    OutId = Id;
                    return true;
                }
            }
            return false;
        }

        // Resources, events and tokens carry no membership, so any overlap on them conflicts.
        static bool Overlaps(const FSystemAccess& A, const FSystemAccess& B, uint32 Id)
        {
            const auto Touches = [Id](const FComponentAccess& Access) { return Access.Id == Id; };
            if (!Algo::AnyOf(A.ComponentAccesses, Touches) || !Algo::AnyOf(B.ComponentAccesses, Touches))
            {
                return true;
            }
            for (const FComponentAccess& First : A.ComponentAccesses)
            {
                if (First.Id != Id)
                {
                    continue;
                }
                for (const FComponentAccess& Second : B.ComponentAccesses)
                {
                    if (Second.Id == Id && (First.bWrite || Second.bWrite) && !A.IsDisjointFrom(First, B, Second))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        NODISCARD bool IsDisjointFrom(const FComponentAccess& Mine, const FSystemAccess& Other, const FComponentAccess& Theirs) const
        {
            if (Mine.Membership == Constants::kIndexNone || Theirs.Membership == Constants::kIndexNone)
            {
                return false;
            }
            return FAccessMembership::AreDisjoint(Memberships[Mine.Membership], Other.Memberships[Theirs.Membership]);
        }

        // An exclusive system implicitly declares everything; otherwise the id must be in the matching set.
        bool DeclaresWrite(uint32 Id) const
        {
            if (bExclusive)
            {
                return true;
            }
            return Algo::Contains(Writes, Id);
        }

        // A write satisfies a read (you may read what you write), so check both sets.
        bool DeclaresRead(uint32 Id) const
        {
            return DeclaresWrite(Id) || Algo::Contains(Reads, Id);
        }
    };

    // Moves the epochs of each untracked pool the access writes, or of every untracked pool for an exclusive system. A tracked pool stamps its own writes.
    inline void MarkDeclaredWrites(ECS::FRegistry& Registry, const FSystemAccess& Access)
    {
        if (Access.bExclusive)
        {
            for (ECS::FSparseSet* Storage : Registry.GetActiveStorages())
            {
                Storage->MarkScheduledWrite();
            }
            return;
        }

        // Resource and event ids share the list but own no pool, so the lookup skips them.
        for (const uint32 Id : Access.Writes)
        {
            if (ECS::FSparseSet* Storage = Registry.FindStorage(Id))
            {
                Storage->MarkScheduledWrite();
            }
        }
    }

    // Development-only check that a running system declared what it touched; a no-op outside the scheduler, for exclusive systems, and in Shipping.
#if defined(LE_SHIPPING)
    inline const FSystemAccess* SetExecutingSystemAccess(const FSystemAccess*) { return nullptr; }
    inline void ValidateSystemAccess(uint32, bool, const char*) {}
    inline void ValidateNotInParallelSystem(const char*) {}
    inline void RegisterComponentAccessValidator(void (*)(ECS::FRegistry&)) {}
    inline void ConnectComponentAccessValidators(ECS::FRegistry&) {}
#else
    // Returns the access it replaced, so a system that helps run another one nested on its thread gets its own back.
    RUNTIME_API const FSystemAccess* SetExecutingSystemAccess(const FSystemAccess* Access);
    RUNTIME_API void ValidateSystemAccess(uint32 ComponentId, bool bWrite, const char* What);

    // For calls that edit shared engine state at once, which a system running alongside others has to defer to the flush instead.
    RUNTIME_API void ValidateNotInParallelSystem(const char* What);
    RUNTIME_API void RegisterComponentAccessValidator(void (*Connect)(ECS::FRegistry&));
    RUNTIME_API void ConnectComponentAccessValidators(ECS::FRegistry& Registry);
#endif

    // A component type id rendered for editor tooling, or NAME_None when the id is unknown.
    RUNTIME_API FName GetAccessTypeName(uint32 Id);
}
