#include "RuntimePCH.h"
#include "HierarchicalTagRegistry.h"

namespace Lumina
{
    FHierarchicalTagRegistry& FHierarchicalTagRegistry::Get()
    {
        static FHierarchicalTagRegistry Instance;
        return Instance;
    }

    FHierarchicalTagRegistry::~FHierarchicalTagRegistry()
    {
        for (std::atomic<FNode*>& Block : Blocks)
        {
            delete[] Block.exchange(nullptr, std::memory_order_relaxed);
        }
    }

    uint32 FHierarchicalTagRegistry::RequestTag(FName Name)
    {
        if (Name.IsNone())
        {
            return 0;
        }

        if (const uint32 Found = FindTag(Name))
        {
            return Found;
        }

        FWriteScopeLock Lock(Mutex);
        return InternLocked(Name);
    }

    uint32 FHierarchicalTagRegistry::FindTag(FName Name) const
    {
        if (Name.IsNone())
        {
            return 0;
        }
        FReadScopeLock Lock(Mutex);
        const uint32* Found = NameToId.Find(Name);
        return Found != nullptr ? *Found : 0;
    }

    uint32 FHierarchicalTagRegistry::GetParent(uint32 Id) const
    {
        return IsValid(Id) ? NodeAt(Id)->ParentId : 0;
    }

    bool FHierarchicalTagRegistry::IsValid(uint32 Id) const
    {
        return Id != 0 && Id < Count.load(std::memory_order_acquire);
    }

    bool FHierarchicalTagRegistry::Matches(uint32 A, uint32 B) const
    {
        // A parent is always registered before its children, so an ancestor's id is never the larger one.
        if (B == 0 || B > A || !IsValid(A))
        {
            return false;
        }
        for (uint32 Current = A; Current >= B; Current = NodeAt(Current)->ParentId)
        {
            if (Current == B)
            {
                return true;
            }
        }
        return false;
    }

    FName FHierarchicalTagRegistry::GetTagName(uint32 Id) const
    {
        return IsValid(Id) ? NodeAt(Id)->Name : FName();
    }

    FString FHierarchicalTagRegistry::GetName(uint32 Id) const
    {
        return IsValid(Id) ? NodeAt(Id)->Name.ToString() : FString();
    }

    void FHierarchicalTagRegistry::GetAllTags(TVector<FString>& Out) const
    {
        const uint32 End = Count.load(std::memory_order_acquire);
        Out.Reserve(Out.Num() + static_cast<int32>(End) - 1);
        for (uint32 Id = 1; Id < End; ++Id)
        {
            Out.Add(NodeAt(Id)->Name.ToString());
        }
    }

    const FHierarchicalTagRegistry::FNode* FHierarchicalTagRegistry::NodeAt(uint32 Id) const
    {
        return Blocks[Id >> kBlockBits].load(std::memory_order_acquire) + (Id & (kBlockSize - 1));
    }

    uint32 FHierarchicalTagRegistry::InternLocked(FName Name)
    {
        if (Name.IsNone())
        {
            return 0;
        }
        if (const uint32* Found = NameToId.Find(Name))
        {
            return *Found;
        }

        const FString Text = Name.ToString();
        const FStringView View = Text.View();
        const int32 Dot = View.FindLastOf('.');
        const uint32 ParentId = Dot != Constants::kIndexNone ? InternLocked(FName(View.Left(Dot))) : 0;

        const uint32 Id = Count.load(std::memory_order_relaxed);
        if (Id >= kMaxBlocks * kBlockSize)
        {
            return 0;
        }

        FNode* Block = Blocks[Id >> kBlockBits].load(std::memory_order_relaxed);
        if (Block == nullptr)
        {
            Block = new FNode[kBlockSize];
            Blocks[Id >> kBlockBits].store(Block, std::memory_order_release);
        }
        Block[Id & (kBlockSize - 1)] = FNode{ Name, ParentId };

        NameToId.Add(Name, Id);
        Count.store(Id + 1, std::memory_order_release);
        return Id;
    }
}
