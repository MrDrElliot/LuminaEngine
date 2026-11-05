#pragma once

#include <atomic>

#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // One table for every module, since Get() lives in Runtime. Ids are dense, so they double as mask bits, and reading a tag by id takes no lock.
    class RUNTIME_API FHierarchicalTagRegistry
    {
    public:

        static FHierarchicalTagRegistry& Get();

        FHierarchicalTagRegistry() = default;
        ~FHierarchicalTagRegistry();

        FHierarchicalTagRegistry(const FHierarchicalTagRegistry&) = delete;
        FHierarchicalTagRegistry& operator=(const FHierarchicalTagRegistry&) = delete;

        uint32 RequestTag(FName Name);

        uint32 FindTag(FName Name) const;

        uint32 GetParent(uint32 Id) const;

        bool IsValid(uint32 Id) const;

        // A is B or a descendant of B. An invalid B never matches.
        bool Matches(uint32 A, uint32 B) const;

        bool MatchesExact(uint32 A, uint32 B) const { return A != 0 && A == B; }

        FName GetTagName(uint32 Id) const;

        FString GetName(uint32 Id) const;

        void GetAllTags(TVector<FString>& Out) const;

    private:

        struct FNode
        {
            FName  Name;
            uint32 ParentId = 0;
        };

        static constexpr uint32 kBlockBits = 8;
        static constexpr uint32 kBlockSize = 1u << kBlockBits;
        static constexpr uint32 kMaxBlocks = 4096;

        const FNode* NodeAt(uint32 Id) const;

        uint32 InternLocked(FName Name);

        mutable FSharedMutex    Mutex;
        THashMap<FName, uint32> NameToId;
        std::atomic<FNode*>     Blocks[kMaxBlocks] = {};
        std::atomic<uint32>     Count { 1 };
    };
}
