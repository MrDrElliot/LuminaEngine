#pragma once

#include <mutex>

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"
#include "Containers/Array.h"

namespace Lumina
{
    // A process-global, hierarchical gameplay-tag table.
    class FGameplayTagRegistry
    {
    public:

        static FGameplayTagRegistry& Get()
        {
            static FGameplayTagRegistry Instance;
            return Instance;
        }

        // Intern a tag and its ancestor chain; returns the id (0 for an empty name).
        uint32 RequestTag(FStringView Name)
        {
            if (Name.empty())
            {
                return 0;
            }
            std::lock_guard<std::mutex> Lock(Mutex);
            return InternLocked(Name);
        }

        // The immediate parent id ("Ability.Fire" -> "Ability"), or 0 at the root / for an invalid id.
        uint32 GetParent(uint32 Id) const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return (Id != 0 && Id < Nodes.size()) ? Nodes[Id].ParentId : 0;
        }

        bool IsValid(uint32 Id) const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return Id != 0 && Id < Nodes.size();
        }

        // A matches B: the same tag, or B is an ancestor of A. An invalid B (0) never matches.
        bool Matches(uint32 A, uint32 B) const
        {
            if (A == 0 || B == 0)
            {
                return false;
            }
            std::lock_guard<std::mutex> Lock(Mutex);
            for (uint32 Cur = A; Cur != 0 && Cur < Nodes.size(); Cur = Nodes[Cur].ParentId)
            {
                if (Cur == B)
                {
                    return true;
                }
            }
            return false;
        }

        bool MatchesExact(uint32 A, uint32 B) const
        {
            return A != 0 && A == B;
        }

        FString GetName(uint32 Id) const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return (Id != 0 && Id < Nodes.size()) ? Nodes[Id].Name : FString();
        }

        // Every registered tag name (skips the index-0 sentinel). For the editor tag picker.
        void GetAllTags(TVector<FString>& Out) const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Out.reserve(Out.size() + Nodes.size());
            for (size_t i = 1; i < Nodes.size(); ++i)
            {
                Out.push_back(Nodes[i].Name);
            }
        }

    private:

        struct FNode
        {
            FString Name;
            uint32  ParentId = 0;
        };

        static uint64 HashTagName(FStringView S)
        {
            uint64 H = 1469598103934665603ull;
            for (char C : S)
            {
                H ^= static_cast<uint8>(C);
                H *= 1099511628211ull;
            }
            return H;
        }

        // Caller holds Mutex. Recurses to intern the ancestor chain before this tag (does NOT re-lock).
        uint32 InternLocked(FStringView Name)
        {
            const uint64 Hash = HashTagName(Name);
            const auto It = HashToId.find(Hash);
            if (It != HashToId.end())
            {
                return It->second;
            }

            uint32 ParentId = 0;
            const size_t Dot = Name.find_last_of('.');
            if (Dot != FStringView::npos)
            {
                ParentId = InternLocked(Name.substr(0, Dot));
            }

            const uint32 Id = static_cast<uint32>(Nodes.size());
            Nodes.push_back(FNode{ FString(Name.data(), Name.size()), ParentId });
            HashToId[Hash] = Id;
            return Id;
        }

        mutable std::mutex          Mutex;
        TVector<FNode>              Nodes = { FNode{} };   // index 0 is the invalid / None sentinel.
        THashMap<uint64, uint32>    HashToId;
    };
}
