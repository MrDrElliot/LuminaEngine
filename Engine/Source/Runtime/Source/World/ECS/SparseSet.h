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
            Func(Entity, Forward<TArgs>(Args)...);
        }
        else
        {
            Func(Forward<TArgs>(Args)...);
        }
    }

    class FSparseSet;

    // Every entity holding all Owned types sits at dense [0, Num) of each owned pool, at the same index in each.
    struct FOwningGroup
    {
        TInlineVector<FSparseSet*, 8> Owned;
        int32 Num = 0;
    };

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
        FSparseSet(FSparseSet&&) = delete;
        FSparseSet& operator=(FSparseSet&&) = delete;


        //~ Queries

        // An empty slot is memset to the null handle, so a null query has to be rejected before the version test.
        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const
        {
            return !Entity.IsNull() && FindSlot(Entity.GetIndex()).GetVersion() == Entity.GetVersion();
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
            if (Page >= static_cast<uint32>(SparsePages.Num()))
            {
                return NullEntity;
            }
            const FEntity* Slots = SparsePages[Page];
            return Slots != nullptr ? Slots[EntityIndex % SparsePageSize] : NullEntity;
        }

        NODISCARD FORCEINLINE int32 Num() const { return LiveCount; }
        NODISCARD FORCEINLINE bool IsEmpty() const { return LiveCount == 0; }

        // Includes tombstones on a stable pool, so index with a tombstone check.
        NODISCARD FORCEINLINE int32 GetDenseSize() const { return Dense.Num(); }
        NODISCARD FORCEINLINE const FEntity* GetDenseData() const { return Dense.GetData(); }

        // The highest entity index in the pool, tombstones skipped, or 0 when it holds none. For sizing an index-keyed table.
        NODISCARD RUNTIME_API uint32 MaxLiveIndex() const;

        // The entity whose payload starts at Payload, or null when the address is not a live element of this pool.
        NODISCARD FEntity FindPayloadOwner(const void* Payload) const;

        NODISCARD FORCEINLINE bool HasTombstones() const { return Dense.Num() != LiveCount; }

        // The group that keeps this pool's dense front packed, or null when no group owns the pool.
        NODISCARD FORCEINLINE const FOwningGroup* GetOwningGroup() const { return OwningGroup; }

        NODISCARD FORCEINLINE const FComponentTypeInfo& GetTypeInfo() const { return TypeInfo; }
        NODISCARD FORCEINLINE FComponentTypeID GetTypeID() const { return TypeInfo.TypeID; }
        NODISCARD CStruct* GetStruct() const { return TypeInfo.GetStruct(); }
        NODISCARD FORCEINLINE const FName& GetName() const { return TypeInfo.Name; }

        // Changes whenever an element may move or die, and outlives the pool to read DeadLayoutEpoch after it.
        NODISCARD FORCEINLINE const uint32* GetLayoutEpoch() const { return LayoutEpoch; }
        static constexpr uint32 DeadLayoutEpoch = 0xFFFFFFFFu;


        //~ Change detection

        // Moves on Emplace, EmplaceOrReplace, Remove, Clear, Compact, Relayout and every stamped write; an untracked pool also moves on each run of a scheduled system that writes it.
        NODISCARD FORCEINLINE uint64 GetWriteEpoch() const { return WriteEpoch.load(std::memory_order_relaxed); }

        // A system that declared the pool written is about to run. Only an untracked pool needs telling, since its writes are not stamped.
        FORCEINLINE void MarkScheduledWrite()
        {
            if (!bTrackChanges)
            {
                BumpWriteEpoch();
            }
        }

        // An in-place write to one live element, which moves the epoch and stamps the element.
        FORCEINLINE void MarkWritten(uint32 DenseIndex)
        {
            BumpWriteEpoch();
            if (bTrackChanges)
            {
                StampChange(DenseIndex);
            }
        }

        // Mutable access to one live element, which a tracked pool counts as a write and an untracked one ignores.
        FORCEINLINE void StampIfTracked(uint32 DenseIndex)
        {
            if (bTrackChanges) [[unlikely]]
            {
                StampChange(DenseIndex);
            }
        }

        // A type-erased GetRaw that counts as mutable access, for a caller about to write through the pointer.
        NODISCARD FORCEINLINE void* GetRawForWrite(FEntity Entity)
        {
            const uint32 DenseIndex = FindLiveDense(Entity);
            if (DenseIndex == InvalidDenseIndex)
            {
                return nullptr;
            }
            StampIfTracked(DenseIndex);
            return GetRawAtDense(DenseIndex);
        }

        // Stamps the entity's element if the pool holds it and is tracked.
        RUNTIME_API void StampEntity(FEntity Entity);

        NODISCARD FORCEINLINE bool IsTrackingChanges() const { return bTrackChanges; }

        // Keeps a change tick per dense slot from now on, stamping every element so the first query reports the whole pool.
        RUNTIME_API void TrackChanges();

        // A pointer into the pool went to a holder that may cache it, so ExpireEscapedPointers has to make it look again. Any thread may mark.
        FORCEINLINE void MarkPointersEscaped()
        {
            if (!bPointersEscaped.load(std::memory_order_relaxed))
            {
                bPointersEscaped.store(true, std::memory_order_relaxed);
            }
        }

        // Moves the layout epoch of a tracked pool whose pointers escaped, so every cached reference resolves, and so stamps, again.
        RUNTIME_API void ExpireEscapedPointers();

        // Parallel to the dense array, or null while untracked, so a parallel walk can test its own slice.
        NODISCARD FORCEINLINE const uint32* GetChangeTickData() const { return bTrackChanges ? ChangeTicks.GetData() : nullptr; }

        // The tick each element was added at, parallel to the dense array like the change ticks.
        NODISCARD FORCEINLINE const uint32* GetAddedTickData() const { return bTrackChanges ? AddedTicks.GetData() : nullptr; }

        // Undefined unless tracked and DenseIndex is a live slot. Any thread may stamp, since a stamp is a relaxed atomic store.
        FORCEINLINE void SetChangeTickAt(uint32 DenseIndex, uint32 Tick)
        {
            std::atomic_ref<uint32>(ChangeTicks[static_cast<int32>(DenseIndex)]).store(Tick, std::memory_order_relaxed);
        }

        // Where a write handle stamps a row; an untracked pool hands back the walk's own sink, so the handle never branches on tracking.
        NODISCARD FORCEINLINE uint32* GetChangeTickSlot(uint32 DenseIndex, uint32& WalkSink)
        {
            return bTrackChanges ? ChangeTicks.GetData() + DenseIndex : &WalkSink;
        }

        // Records that a stamp of this tick is about to land, moving the epoch once per tick rather than once per stamp.
        FORCEINLINE void NoteStampTick(uint32 Tick)
        {
            if (LastStampTick.load(std::memory_order_relaxed) != Tick)
            {
                LastStampTick.store(Tick, std::memory_order_relaxed);
                BumpWriteEpoch();
            }
        }

        RUNTIME_API void StampAllChanges(uint32 Tick);

        // Wrap-safe, so a stamp from before a counter wrap reads as changed rather than as old.
        NODISCARD static constexpr bool IsNewerTick(uint32 Stamp, uint32 Since) { return static_cast<int32>(Stamp - Since) > 0; }

        // Always true for a held element of an untracked pool, so a reader that never enabled tracking stays correct.
        NODISCARD FORCEINLINE bool HasChangedSince(FEntity Entity, uint32 Since) const { return HasTickSince(ChangeTicks, Entity, Since); }
        NODISCARD FORCEINLINE bool HasAddedSince(FEntity Entity, uint32 Since) const { return HasTickSince(AddedTicks, Entity, Since); }

        // The live dense index of Entity, or InvalidDenseIndex for a null, absent or stale handle.
        NODISCARD FORCEINLINE uint32 FindLiveDense(FEntity Entity) const
        {
            if (Entity.IsNull())
            {
                return InvalidDenseIndex;
            }
            const FEntity Slot = FindSlot(Entity.GetIndex());
            return Slot.GetVersion() == Entity.GetVersion() ? Slot.GetIndex() : InvalidDenseIndex;
        }

        NODISCARD FORCEINLINE bool HasTickSince(const TVector<uint32>& Ticks, FEntity Entity, uint32 Since) const
        {
            const uint32 DenseIndex = FindLiveDense(Entity);
            return DenseIndex != InvalidDenseIndex && (!bTrackChanges || IsNewerTick(Ticks[static_cast<int32>(DenseIndex)], Since));
        }

        // Process-wide, so stamps stay comparable across a registry swap.
        NODISCARD static RUNTIME_API uint32 GetChangeTick();

        // Hands back the current tick and moves on, so every later write stamps newer than the value returned.
        static RUNTIME_API uint32 AdvanceChangeTick();

        NODISCARD FORCEINLINE bool IsPaged() const { return bPaged; }
        NODISCARD FORCEINLINE uint32 GetPayloadPageSize() const { return PayloadPageMask + 1u; }
        NODISCARD FORCEINLINE bool IsTagOnly() const { return ElementSize == 0; }
        NODISCARD FORCEINLINE uint32 GetElementSize() const { return ElementSize; }

        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const FSparseSet* InSet, int32 InEndSlot)
                : Set(InSet)
                , EndSlot(InEndSlot)
            {
                Advance();
            }

            NODISCARD FEntity operator * () const { return Set->Dense[EndSlot - 1]; }

            FIterator& operator ++ () { --EndSlot; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return EndSlot == Other.EndSlot; }
            NODISCARD bool operator != (const FIterator& Other) const { return EndSlot != Other.EndSlot; }

        private:

            void Advance()
            {
                if (Set == nullptr)
                {
                    return;
                }

                while (EndSlot > 0 && Set->Dense[EndSlot - 1].IsTombstone())
                {
                    --EndSlot;
                }
            }

            const FSparseSet* Set = nullptr;

            // One past the current slot, so the finished walk sits at zero without wrapping.
            int32 EndSlot = 0;
        };

        // Backward, so removing the current entity only pulls an already visited one into its slot.
        NODISCARD FIterator begin() const { return FIterator(this, Dense.Num()); }
        NODISCARD FIterator end() const { return FIterator(this, 0); }


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
            if (Entity.IsNull())
            {
                return nullptr;
            }
            const FEntity Slot = FindSlot(Entity.GetIndex());
            if (Slot.GetVersion() != Entity.GetVersion())
            {
                return nullptr;
            }
            return GetRawAtDense(Slot.GetIndex());
        }

        NODISCARD FORCEINLINE FMembershipProbe MakeProbe() const
        {
            return FMembershipProbe{ SparsePages.GetData(), static_cast<size_t>(SparsePages.Num()) };
        }

        // The block a typed accessor reinterprets. Only one of these is meaningful, per IsPaged.
        NODISCARD FORCEINLINE uint8* GetPackedBlock() const { return PackedData; }
        NODISCARD FORCEINLINE uint8* GetPayloadPage(int32 PageIndex) const { return PayloadPages[PageIndex]; }


        //~ Mutation

        // Inline, because an opaque call here stops the caller's element stores pipelining across iterations.
        NODISCARD FORCEINLINE uint32 AllocateSlot(FEntity Entity)
        {
            BumpWriteEpoch();
            if (FreeDenseHead == InvalidDenseIndex && !bTrackChanges && OwningGroup == nullptr)
            {
                const uint32 EntityIndex = Entity.GetIndex();
                const uint32 Page = EntityIndex / SparsePageSize;

                if (Page < static_cast<uint32>(SparsePages.Num()))
                {
                    FEntity* Slots = SparsePages[Page];
                    const int32 Next = Dense.Num();

                    if (Slots != nullptr && Next < Dense.Max() && HasPayloadRoomFor(Next))
                    {
                        Dense.Add(Entity);
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

        RUNTIME_API void Reserve(int32 Count);

        // Rebuilds the payload in a new layout of the same type, keeping every entity at its dense index.
        RUNTIME_API void Relayout(const FComponentTypeInfo& NewInfo, FMigrateComponentValue Migrate);

        // Lives on the pool rather than beside it, so a type-erased write fires the same hooks a typed one does.
        FComponentSignals Signals;

    private:

        friend class FRegistry;

        RUNTIME_API FEntity& AssureSlot(uint32 EntityIndex);

        RUNTIME_API uint32 AllocateSlotSlow(FEntity Entity);

        RUNTIME_API void StampChange(uint32 DenseIndex);

        // Exchanges two live elements with their entities, sparse slots and ticks.
        RUNTIME_API void SwapDenseSlots(uint32 A, uint32 B);

        // Moves the live element at From into the unconstructed slot To, leaving From unconstructed.
        void MoveDenseSlot(uint32 From, uint32 To);

        void RelocateElement(void* Target, void* Source) const;

        // Called with the new entity's payload still unconstructed, so it may hand back a different slot for the caller to fill.
        uint32 JoinOwningGroup(FEntity Entity, uint32 DenseIndex);
        void LeaveOwningGroup(FEntity Entity);

        // Relaxed, because a mutable Get on a tracked pool may move it from a parallel system.
        FORCEINLINE void BumpWriteEpoch() { WriteEpoch.store(WriteEpoch.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed); }

        NODISCARD FORCEINLINE bool HasPayloadRoomFor(int32 DenseIndex) const
        {
            if (ElementSize == 0)
            {
                return true;
            }
            return bPaged ? DenseIndex < (PayloadPages.Num() << PayloadPageShift) : DenseIndex < PackedCapacity;
        }

        // Unlinks the entity and hands back its dense slot, or InvalidDenseIndex when absent.
        RUNTIME_API uint32 ReleaseSlot(FEntity Entity);

        void ApplyLayout(const FComponentTypeInfo& InTypeInfo);
        RUNTIME_API void GrowPayloadTo(int32 ElementCount);
        RUNTIME_API void BumpLayoutEpoch();
        RUNTIME_API void DestroyLiveElements();
        RUNTIME_API void FreePayload();

        FComponentTypeInfo TypeInfo;

        uint32* LayoutEpoch = nullptr;

        // Starts in a range of its own per pool, so a swapped-in pool never repeats an epoch a reader cached.
        std::atomic<uint64> WriteEpoch{ 0 };
        std::atomic<uint32> LastStampTick{ 0 };

        // Beside the sparse pages, so the test every Get pays shares their cache line.
        bool bTrackChanges = false;
        std::atomic<bool> bPointersEscaped{ false };

        TVector<FEntity*> SparsePages;
        TVector<FEntity> Dense;

        // Empty unless bTrackChanges, and then always as long as Dense.
        TVector<uint32> ChangeTicks;
        TVector<uint32> AddedTicks;

        // One growing block for a packed pool. Null when the pool is paged or holds a tag.
        uint8* PackedData = nullptr;
        int32 PackedCapacity = 0;

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

        // Dense minus tombstones, so a packed pool sees Dense.Num() and a stable one does not.
        int32 LiveCount = 0;

        // Head of the tombstone chain threaded through Dense. InvalidDenseIndex when there is none.
        uint32 FreeDenseHead = InvalidDenseIndex;

        FOwningGroup* OwningGroup = nullptr;
    };
}

static_assert(Lumina::ECS::FSparseSet::SparsePageSize == 4096,
    "FMembershipProbe hard-codes the page size so the divide folds to a shift.");
