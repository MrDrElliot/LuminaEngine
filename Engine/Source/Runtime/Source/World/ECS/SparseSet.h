#pragma once

#include "ComponentType.h"
#include "Entity.h"
#include "Signal.h"
#include "Containers/Vector.h"
#include "Memory/Memory.h"

namespace Lumina::ECS
{
    template<typename TFunc, typename... TArgs>
    FORCEINLINE void InvokeEntityCallback(TFunc& Func, FEntity Entity, TArgs&&... Args)
    {
        if constexpr (std::is_invocable_v<TFunc&, FEntity, TArgs...>)
        {
            Func(Entity, std::forward<TArgs>(Args)...);
        }
        else
        {
            Func(std::forward<TArgs>(Args)...);
        }
    }

    class FSparseSet;

    // A pool's sparse pages hoisted into registers, so a filter loop reloads nothing through the pool.
    struct FMembershipProbe
    {
        const FEntity* const* Pages = nullptr;
        size_t PageCount = 0;

        NODISCARD constexpr bool Contains(FEntity Entity) const
        {
            const uint32 EntityIndex = Entity.GetIndex();
            const uint32 Page = EntityIndex / 4096u;
            if (Page >= PageCount)
            {
                return false;
            }
            const FEntity* Slots = Pages[Page];
            return Slots != nullptr && Slots[EntityIndex % 4096u].GetVersion() == Entity.GetVersion();
        }
    };

    static_assert(std::is_trivially_copyable_v<FMembershipProbe>, "a probe is copied into every filter loop.");

    // One component pool, entity mapping and payload together. No virtuals, so nothing here dispatches.
    class FSparseSet
    {
    public:

        // 4096 uint32 slots is one 16 KiB page, which keeps a sparse world from paying for a flat array.
        static constexpr uint32 SparsePageSize = 4096;

        // Payload pages are sized per type, from TComponentTraits<T>::PageSize.

        // IndexMask, not ~0u, because the chain terminator round-trips through a tombstone's 20-bit index.
        static constexpr uint32 InvalidDenseIndex = FEntity::IndexMask;

        // A slot packs the dense index with the owner version, and a 0xFF memset leaves the empty marker.
        static constexpr uint32 MaxDenseIndex = FEntity::IndexMask - 1u;

        RUNTIME_API explicit FSparseSet(const FComponentTypeInfo& InTypeInfo);
        RUNTIME_API ~FSparseSet();

        FSparseSet(const FSparseSet&) = delete;
        FSparseSet& operator = (const FSparseSet&) = delete;


        //~ Queries

        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const
        {
            return FindSlot(Entity.GetIndex()).GetVersion() == Entity.GetVersion();
        }

        // Undefined unless Contains, so the hot path can skip the double lookup.
        NODISCARD FORCEINLINE uint32 GetDenseIndex(FEntity Entity) const
        {
            return FindSlot(Entity.GetIndex()).GetIndex();
        }

        // The packed slot, so a caller that needs both the membership test and the index pays one load.
        NODISCARD FORCEINLINE FEntity FindSlot(uint32 EntityIndex) const
        {
            const uint32 Page = EntityIndex / SparsePageSize;
            if (Page >= SparsePages.size())
            {
                return NullEntity;
            }
            const FEntity* Slots = SparsePages[Page];
            return Slots != nullptr ? Slots[EntityIndex % SparsePageSize] : NullEntity;
        }

        NODISCARD FORCEINLINE size_t Num() const { return LiveCount; }
        NODISCARD FORCEINLINE bool IsEmpty() const { return LiveCount == 0; }

        // Includes tombstones on a stable pool, so index with a tombstone check.
        NODISCARD FORCEINLINE size_t GetDenseSize() const { return Dense.size(); }
        NODISCARD FORCEINLINE const FEntity* GetDenseData() const { return Dense.data(); }
        NODISCARD FORCEINLINE FEntity GetDenseAt(size_t Index) const { return Dense[Index]; }

        NODISCARD FORCEINLINE bool HasTombstones() const { return Dense.size() != LiveCount; }

        NODISCARD FORCEINLINE const FComponentTypeInfo& GetTypeInfo() const { return TypeInfo; }
        NODISCARD FORCEINLINE FComponentTypeID GetTypeID() const { return TypeInfo.TypeID; }
        NODISCARD CStruct* GetStruct() const { return TypeInfo.GetStruct(); }
        NODISCARD FORCEINLINE const FName& GetName() const { return TypeInfo.Name; }

        NODISCARD FORCEINLINE bool IsPaged() const { return bPaged; }
        NODISCARD FORCEINLINE bool IsInPlaceDelete() const { return bInPlaceDelete; }
        NODISCARD FORCEINLINE uint32 GetPayloadPageSize() const { return PayloadPageMask + 1u; }
        NODISCARD FORCEINLINE bool IsTagOnly() const { return ElementSize == 0; }

        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const FSparseSet* InSet, size_t InIndex)
                : Set(InSet)
                , Index(InIndex)
            {
                Advance();
            }

            NODISCARD FEntity operator * () const { return Set->Dense[Index]; }

            FIterator& operator ++ () { ++Index; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return Index == Other.Index; }
            NODISCARD bool operator != (const FIterator& Other) const { return Index != Other.Index; }

        private:

            void Advance()
            {
                if (Set == nullptr)
                {
                    return;
                }

                while (Index < Set->Dense.size() && Set->Dense[Index].IsTombstone())
                {
                    ++Index;
                }
            }

            const FSparseSet* Set = nullptr;
            size_t Index = 0;
        };

        NODISCARD FIterator begin() const { return FIterator(this, 0); }
        NODISCARD FIterator end() const { return FIterator(this, Dense.size()); }


        //~ Element access. The payload lives here, so none of this dispatches.

        NODISCARD FORCEINLINE void* GetRawAtDense(uint32 DenseIndex) const
        {
            if (ElementSize == 0)
            {
                return nullptr;
            }
            if (bPaged)
            {
                return PayloadPages[DenseIndex >> PayloadPageShift]
                    + static_cast<size_t>(DenseIndex & PayloadPageMask) * ElementSize;
            }
            return PackedData + static_cast<size_t>(DenseIndex) * ElementSize;
        }

        NODISCARD FORCEINLINE void* GetRaw(FEntity Entity) const
        {
            const FEntity Slot = FindSlot(Entity.GetIndex());
            if (Slot.GetVersion() != Entity.GetVersion())
            {
                return nullptr;
            }
            return GetRawAtDense(Slot.GetIndex());
        }

        NODISCARD FORCEINLINE FMembershipProbe MakeProbe() const
        {
            return FMembershipProbe{ SparsePages.data(), SparsePages.size() };
        }

        // The block a typed accessor reinterprets. Only one of these is meaningful, per IsPaged.
        NODISCARD FORCEINLINE uint8* GetPackedBlock() const { return PackedData; }
        NODISCARD FORCEINLINE uint8* GetPayloadPage(size_t PageIndex) const { return PayloadPages[PageIndex]; }


        //~ Mutation

        // Inline, because an opaque call here stops the caller's element stores pipelining across iterations.
        NODISCARD FORCEINLINE uint32 AllocateSlot(FEntity Entity)
        {
            if (FreeDenseHead == InvalidDenseIndex)
            {
                const uint32 EntityIndex = Entity.GetIndex();
                const uint32 Page = EntityIndex / SparsePageSize;

                if (Page < SparsePages.size())
                {
                    FEntity* Slots = SparsePages[Page];
                    const size_t Next = Dense.size();

                    if (Slots != nullptr && Next < Dense.capacity() && HasPayloadRoomFor(Next))
                    {
                        Dense.push_back(Entity);
                        Slots[EntityIndex % SparsePageSize] = FEntity(static_cast<uint32>(Next), Entity.GetVersion());
                        ++LiveCount;
                        return static_cast<uint32>(Next);
                    }
                }
            }

            return AllocateSlotSlow(Entity);
        }

        RUNTIME_API bool RemoveEntity(FEntity Entity);
        RUNTIME_API void ClearAll();

        // Default-constructs when absent and overwrites when present, mirroring emplace_or_replace.
        RUNTIME_API void* EmplaceDefaultRaw(FEntity Entity);
        RUNTIME_API void* EmplaceCopyRaw(FEntity Entity, const void* Source);

        // Drops tombstones so iteration stops paying for them. Invalidates every dense index.
        RUNTIME_API void Compact();

        RUNTIME_API void Reserve(size_t Count);

        // Lives on the pool rather than beside it, so a type-erased write fires the same hooks a typed one does.
        FComponentSignals Signals;

    private:

        friend class FRegistry;

        RUNTIME_API FEntity& AssureSlot(uint32 EntityIndex);

        RUNTIME_API uint32 AllocateSlotSlow(FEntity Entity);

        NODISCARD FORCEINLINE bool HasPayloadRoomFor(size_t DenseIndex) const
        {
            if (ElementSize == 0)
            {
                return true;
            }
            return bPaged ? DenseIndex < (PayloadPages.size() << PayloadPageShift) : DenseIndex < PackedCapacity;
        }

        // Unlinks the entity and hands back its dense slot, or InvalidDenseIndex when absent.
        RUNTIME_API uint32 ReleaseSlot(FEntity Entity);

        RUNTIME_API void GrowPayloadTo(size_t ElementCount);
        RUNTIME_API void DestroyLiveElements();
        RUNTIME_API void FreePayload();

        FComponentTypeInfo TypeInfo;

        TVector<FEntity*> SparsePages;
        TVector<FEntity> Dense;

        // One growing block for a packed pool. Null when the pool is paged or holds a tag.
        uint8* PackedData = nullptr;
        size_t PackedCapacity = 0;

        // One block per page for a paged pool, so growth allocates a page instead of relocating the payload.
        TVector<uint8*> PayloadPages;

        uint32 ElementSize = 0;
        uint32 ElementAlignment = 0;

        // Derived from the type's PageSize, which the traits prove is a power of two.
        uint32 PayloadPageShift = 0;
        uint32 PayloadPageMask = 0;

        // Layout and removal are independent. Paged avoids relocation, in-place delete avoids moving an element.
        bool   bPaged = false;
        bool   bInPlaceDelete = false;

        // Dense minus tombstones, so a packed pool sees Dense.size() and a stable one does not.
        size_t LiveCount = 0;

        // Head of the tombstone chain threaded through Dense. InvalidDenseIndex when there is none.
        uint32 FreeDenseHead = InvalidDenseIndex;
    };
}

static_assert(Lumina::ECS::FSparseSet::SparsePageSize == 4096,
    "FMembershipProbe hard-codes the page size so the divide folds to a shift.");
