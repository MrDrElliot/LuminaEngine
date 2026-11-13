#include "RuntimePCH.h"
#include "SparseSet.h"

#include "Core/Assertions/Assert.h"

namespace Lumina::ECS
{
    namespace SparseSetInternal
    {
        constexpr int32 MinPackedCapacity = 8;

        // A payload block under-aligned to its element straddles cache lines on every other store.
        constexpr size_t MinPayloadAlignment = 16;

        NODISCARD constexpr size_t PayloadAlignment(uint32 ElementAlignment)
        {
            return ElementAlignment > MinPayloadAlignment ? ElementAlignment : MinPayloadAlignment;
        }
    }

    uint32 FSparseSet::GetChangeTick()
    {
        return FComponentTypeRegistry::Get().GetChangeTick();
    }

    uint32 FSparseSet::AdvanceChangeTick()
    {
        return FComponentTypeRegistry::Get().AdvanceChangeTick();
    }

    FSparseSet::FSparseSet(const FComponentTypeInfo& InTypeInfo)
    {
        ApplyLayout(InTypeInfo);
        WriteEpoch.store(FComponentTypeRegistry::Get().AcquireWriteEpochBase(), std::memory_order_relaxed);
        LayoutEpoch = FComponentTypeRegistry::Get().AcquireLayoutEpochSlot();
        if (TypeInfo.bRuntimeLayout)
        {
            FComponentTypeRegistry::Get().TrackPool(this);
        }
    }

    FSparseSet::~FSparseSet()
    {
        if (TypeInfo.bRuntimeLayout)
        {
            FComponentTypeRegistry::Get().UntrackPool(this);
        }
        *LayoutEpoch = DeadLayoutEpoch;
        DestroyLiveElements();
        FreePayload();

        for (FEntity* Page : SparsePages)
        {
            if (Page != nullptr)
            {
                void* Block = Page;
                Memory::Free(Block);
            }
        }
    }

    uint32 FSparseSet::MaxLiveIndex() const
    {
        const uint32* Packed = reinterpret_cast<const uint32*>(Dense.GetData());
        const int32  Count  = Dense.Num();

        const __m256i IndexMask    = _mm256_set1_epi32((int32)FEntity::IndexMask);
        const __m256i TombstoneTag = _mm256_set1_epi32((int32)(FEntity::TombstoneVersion << FEntity::IndexBits));
        __m256i Highest = _mm256_setzero_si256();

        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const __m256i Handles     = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Packed + i));
            const __m256i bTombstone  = _mm256_cmpeq_epi32(_mm256_andnot_si256(IndexMask, Handles), TombstoneTag);
            const __m256i LiveIndices = _mm256_andnot_si256(bTombstone, _mm256_and_si256(Handles, IndexMask));
            Highest = _mm256_max_epu32(Highest, LiveIndices);
        }

        __m128i Folded = _mm_max_epu32(_mm256_castsi256_si128(Highest), _mm256_extracti128_si256(Highest, 1));
        Folded = _mm_max_epu32(Folded, _mm_shuffle_epi32(Folded, _MM_SHUFFLE(1, 0, 3, 2)));
        Folded = _mm_max_epu32(Folded, _mm_shuffle_epi32(Folded, _MM_SHUFFLE(2, 3, 0, 1)));
        uint32 Result = (uint32)_mm_cvtsi128_si32(Folded);

        for (; i < Count; ++i)
        {
            if (!Dense[i].IsTombstone())
            {
                Result = Math::Max(Result, Dense[i].GetIndex());
            }
        }
        return Result;
    }

    FEntity FSparseSet::FindPayloadOwner(const void* Payload) const
    {
        if (Payload == nullptr || ElementSize == 0)
        {
            return NullEntity;
        }

        const uintptr_t Address = reinterpret_cast<uintptr_t>(Payload);
        int32 DenseIndex = Dense.Num();
        if (bPaged)
        {
            const size_t PageBytes = static_cast<size_t>(PayloadPageMask + 1u) * ElementSize;
            for (int32 Page = 0; Page < PayloadPages.Num(); ++Page)
            {
                const uintptr_t Begin = reinterpret_cast<uintptr_t>(PayloadPages[Page]);
                if (Begin != 0 && Address >= Begin && Address < Begin + PageBytes)
                {
                    const size_t Offset = Address - Begin;
                    if (Offset % ElementSize == 0)
                    {
                        DenseIndex = (Page << PayloadPageShift) + static_cast<int32>(Offset / ElementSize);
                    }
                    break;
                }
            }
        }
        else
        {
            const uintptr_t Begin = reinterpret_cast<uintptr_t>(PackedData);
            if (Begin != 0 && Address >= Begin && Address < Begin + Dense.Num() * ElementSize)
            {
                const size_t Offset = Address - Begin;
                if (Offset % ElementSize == 0)
                {
                    DenseIndex = static_cast<int32>(Offset / ElementSize);
                }
            }
        }

        if (DenseIndex >= Dense.Num() || Dense[DenseIndex].IsTombstone())
        {
            return NullEntity;
        }
        return Dense[DenseIndex];
    }

    void FSparseSet::ApplyLayout(const FComponentTypeInfo& InTypeInfo)
    {
        TypeInfo         = InTypeInfo;
        ElementSize      = InTypeInfo.bEmpty ? 0u : InTypeInfo.Size;
        ElementAlignment = InTypeInfo.Alignment;
        bPaged           = InTypeInfo.bPaged && !InTypeInfo.bEmpty;
        bInPlaceDelete   = InTypeInfo.bInPlaceDelete;

        const uint32 RequestedPageSize = InTypeInfo.PageSize != 0u ? InTypeInfo.PageSize : 1024u;
        DEBUG_ASSERT((RequestedPageSize & (RequestedPageSize - 1u)) == 0u, "page size must be a power of two");

        PayloadPageShift = 0;
        while ((1u << PayloadPageShift) < RequestedPageSize)
        {
            ++PayloadPageShift;
        }
        PayloadPageMask = RequestedPageSize - 1u;
    }

    void FSparseSet::Relayout(const FComponentTypeInfo& NewInfo, FMigrateComponentValue Migrate)
    {
        // The entity mapping and dense order stay, so only the payload is rebuilt and every dense index still holds.
        const FComponentTypeInfo OldInfo = TypeInfo;
        const uint32 OldElementSize = ElementSize;
        const bool   bOldPaged = bPaged;
        const uint32 OldPageShift = PayloadPageShift;
        const uint32 OldPageMask = PayloadPageMask;
        uint8* OldPacked = PackedData;
        TVector<uint8*> OldPages = Move(PayloadPages);

        auto OldValueAt = [&](uint32 DenseIndex) -> uint8*
        {
            if (OldElementSize == 0)
            {
                return nullptr;
            }
            return bOldPaged
                ? OldPages[DenseIndex >> OldPageShift] + static_cast<size_t>(DenseIndex & OldPageMask) * OldElementSize
                : OldPacked + static_cast<size_t>(DenseIndex) * OldElementSize;
        };

        PackedData = nullptr;
        PackedCapacity = 0;
        PayloadPages.Reset();
        ApplyLayout(NewInfo);

        // Allocated outright, since growing a packed block would relocate elements this payload does not hold yet.
        if (ElementSize != 0 && !Dense.IsEmpty())
        {
            if (bPaged)
            {
                GrowPayloadTo(Dense.Num());
            }
            else
            {
                PackedCapacity = Math::Max(Dense.Num(), SparseSetInternal::MinPackedCapacity);
                PackedData = static_cast<uint8*>(Memory::Malloc(static_cast<size_t>(PackedCapacity) * ElementSize, SparseSetInternal::PayloadAlignment(ElementAlignment)));
            }
        }

        for (int32 Index = 0; Index < Dense.Num(); ++Index)
        {
            if (Dense[Index].IsTombstone())
            {
                continue;
            }
            uint8* OldValue = OldValueAt(Index);
            if (void* NewValue = GetRawAtDense(Index))
            {
                TypeInfo.DefaultConstruct(TypeInfo, NewValue);
                if (OldValue != nullptr && Migrate != nullptr)
                {
                    Migrate(OldInfo, OldValue, TypeInfo, NewValue);
                }
            }
            if (OldValue != nullptr && !OldInfo.bTriviallyDestructible && OldInfo.Destruct != nullptr)
            {
                OldInfo.Destruct(OldInfo, OldValue);
            }
        }

        if (OldPacked != nullptr)
        {
            void* Block = OldPacked;
            Memory::Free(Block);
        }
        for (uint8* Page : OldPages)
        {
            if (Page != nullptr)
            {
                void* Block = Page;
                Memory::Free(Block);
            }
        }

        BumpLayoutEpoch();
        BumpWriteEpoch();
        if (bTrackChanges)
        {
            StampAllChanges(GetChangeTick());
        }
    }

    void FSparseSet::TrackChanges()
    {
        if (bTrackChanges)
        {
            return;
        }
        bTrackChanges = true;
        const uint32 Tick = GetChangeTick();
        ChangeTicks.Reserve(Dense.Max());
        ChangeTicks.Init(Tick, Dense.Num());
        AddedTicks.Reserve(Dense.Max());
        AddedTicks.Init(Tick, Dense.Num());
        NoteStampTick(Tick);
    }

    void FSparseSet::StampChange(uint32 DenseIndex)
    {
        const uint32 Tick = GetChangeTick();
        SetChangeTickAt(DenseIndex, Tick);
        NoteStampTick(Tick);
    }

    void FSparseSet::StampEntity(FEntity Entity)
    {
        if (bTrackChanges && Contains(Entity))
        {
            StampChange(GetDenseIndex(Entity));
        }
    }

    void FSparseSet::StampAllChanges(uint32 Tick)
    {
        NoteStampTick(Tick);
        uint32* Ticks = ChangeTicks.GetData();
        const int32 Count = ChangeTicks.Num();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            std::atomic_ref<uint32>(Ticks[Index]).store(Tick, std::memory_order_relaxed);
        }
    }

    void FSparseSet::ExpireEscapedPointers()
    {
        if (bTrackChanges && bPointersEscaped.load(std::memory_order_relaxed))
        {
            BumpLayoutEpoch();
        }
    }

    void FSparseSet::FreePayload()
    {
        if (PackedData != nullptr)
        {
            void* Block = PackedData;
            Memory::Free(Block);
            PackedData = nullptr;
        }
        PackedCapacity = 0;

        for (uint8* Page : PayloadPages)
        {
            if (Page != nullptr)
            {
                void* Block = Page;
                Memory::Free(Block);
            }
        }
        PayloadPages.Reset();
    }

    void FSparseSet::DestroyLiveElements()
    {
        if (ElementSize == 0 || TypeInfo.bTriviallyDestructible || TypeInfo.Destruct == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < Dense.Num(); ++Index)
        {
            if (!Dense[Index].IsTombstone())
            {
                TypeInfo.Destruct(TypeInfo, GetRawAtDense(Index));
            }
        }
    }

    FEntity& FSparseSet::AssureSlot(uint32 EntityIndex)
    {
        const uint32 Page = EntityIndex / SparsePageSize;
        if (Page >= static_cast<uint32>(SparsePages.Num()))
        {
            SparsePages.SetNum(Page + 1u, nullptr);
        }

        FEntity*& Slots = SparsePages[Page];
        if (Slots == nullptr)
        {
            Slots = static_cast<FEntity*>(Memory::Malloc(sizeof(FEntity) * SparsePageSize, alignof(FEntity)));

            // All ones is the reserved tombstone version, which no live entity carries, so it reads as absent.
            Memory::Memset(Slots, 0xFF, sizeof(FEntity) * SparsePageSize);
        }

        return Slots[EntityIndex % SparsePageSize];
    }

    void FSparseSet::BumpLayoutEpoch()
    {
        const uint32 Next = *LayoutEpoch + 1u;
        *LayoutEpoch = Next == DeadLayoutEpoch ? 1u : Next;
    }

    void FSparseSet::GrowPayloadTo(int32 ElementCount)
    {
        if (ElementSize == 0)
        {
            return;
        }

        if (bPaged)
        {
            const int32 PageSize = static_cast<int32>(PayloadPageMask) + 1;
            const int32 NeededPages = (ElementCount + PageSize - 1) / PageSize;
            while (PayloadPages.Num() < NeededPages)
            {
                PayloadPages.Add(static_cast<uint8*>(
                    Memory::Malloc(static_cast<size_t>(ElementSize) * PageSize, SparseSetInternal::PayloadAlignment(ElementAlignment))));
            }
            return;
        }

        if (ElementCount <= PackedCapacity)
        {
            return;
        }

        int32 NewCapacity = Math::Max(PackedCapacity, SparseSetInternal::MinPackedCapacity);
        while (NewCapacity < ElementCount)
        {
            NewCapacity *= 2;
        }

        const uint8* PreviousBlock = PackedData;

        // Realloc first, because rpmalloc can often widen the block in place and skip the copy entirely.
        if (TypeInfo.bTriviallyRelocatable || TypeInfo.Relocate == nullptr)
        {
            PackedData = static_cast<uint8*>(
                Memory::Realloc(PackedData, static_cast<size_t>(NewCapacity) * ElementSize, SparseSetInternal::PayloadAlignment(ElementAlignment)));
        }
        else
        {
            uint8* NewBlock = static_cast<uint8*>(Memory::Malloc(static_cast<size_t>(NewCapacity) * ElementSize, SparseSetInternal::PayloadAlignment(ElementAlignment)));

            // Dense.Num() is the constructed count, which is why growth has to happen before a push.
            for (int32 Index = 0; Index < Dense.Num(); ++Index)
            {
                TypeInfo.Relocate(TypeInfo, NewBlock + Index * ElementSize, PackedData + Index * ElementSize);
            }
            if (PackedData != nullptr)
            {
                void* Block = PackedData;
                Memory::Free(Block);
            }
            PackedData = NewBlock;
        }

        if (PackedData != PreviousBlock)
        {
            BumpLayoutEpoch();
        }

        PackedCapacity = NewCapacity;
    }

    void FSparseSet::Reserve(int32 Count)
    {
        Dense.Reserve(Count);
        if (bTrackChanges)
        {
            ChangeTicks.Reserve(Count);
            AddedTicks.Reserve(Count);
        }
        GrowPayloadTo(Count);
    }

    uint32 FSparseSet::AllocateSlotSlow(FEntity Entity)
    {
        FEntity& Slot = AssureSlot(Entity.GetIndex());
        ASSERT(Slot.GetVersion() != Entity.GetVersion());

        uint32 DenseIndex;
        if (FreeDenseHead != InvalidDenseIndex)
        {
            DenseIndex = FreeDenseHead;
            FreeDenseHead = Dense[DenseIndex].GetIndex();
            Dense[DenseIndex] = Entity;
            if (bTrackChanges)
            {
                const uint32 Tick = GetChangeTick();
                ChangeTicks[static_cast<int32>(DenseIndex)] = Tick;
                AddedTicks[static_cast<int32>(DenseIndex)] = Tick;
            }
        }
        else
        {
            DenseIndex = static_cast<uint32>(Dense.Num());
            ASSERT(DenseIndex <= MaxDenseIndex);

            // Grows before the push, so the relocation loop only ever sees constructed elements.
            GrowPayloadTo(static_cast<int32>(DenseIndex) + 1);
            Dense.Add(Entity);
            if (bTrackChanges)
            {
                const uint32 Tick = GetChangeTick();
                ChangeTicks.Add(Tick);
                AddedTicks.Add(Tick);
            }
        }

        Slot = FEntity(DenseIndex, Entity.GetVersion());
        ++LiveCount;
        return OwningGroup != nullptr ? JoinOwningGroup(Entity, DenseIndex) : DenseIndex;
    }

    void FSparseSet::RelocateElement(void* Target, void* Source) const
    {
        if (TypeInfo.Relocate != nullptr)
        {
            TypeInfo.Relocate(TypeInfo, Target, Source);
        }
        else
        {
            Memory::Memcpy(Target, Source, ElementSize);
        }
    }

    void FSparseSet::SwapDenseSlots(uint32 A, uint32 B)
    {
        if (A == B)
        {
            return;
        }

        if (ElementSize != 0)
        {
            alignas(64) uint8 InlineTemp[256];
            const bool bInline = ElementSize <= sizeof(InlineTemp) && ElementAlignment <= 64u;
            void* Temp = bInline ? static_cast<void*>(InlineTemp)
                : Memory::Malloc(ElementSize, SparseSetInternal::PayloadAlignment(ElementAlignment));
            RelocateElement(Temp, GetRawAtDense(A));
            RelocateElement(GetRawAtDense(A), GetRawAtDense(B));
            RelocateElement(GetRawAtDense(B), Temp);
            if (!bInline)
            {
                Memory::Free(Temp);
            }
        }

        const FEntity EntityA = Dense[static_cast<int32>(A)];
        const FEntity EntityB = Dense[static_cast<int32>(B)];
        Dense[static_cast<int32>(A)] = EntityB;
        Dense[static_cast<int32>(B)] = EntityA;
        AssureSlot(EntityA.GetIndex()) = FEntity(B, EntityA.GetVersion());
        AssureSlot(EntityB.GetIndex()) = FEntity(A, EntityB.GetVersion());
        if (bTrackChanges)
        {
            ChangeTicks.Swap(static_cast<int32>(A), static_cast<int32>(B));
            AddedTicks.Swap(static_cast<int32>(A), static_cast<int32>(B));
        }
        BumpLayoutEpoch();
    }

    void FSparseSet::MoveDenseSlot(uint32 From, uint32 To)
    {
        if (ElementSize != 0)
        {
            RelocateElement(GetRawAtDense(To), GetRawAtDense(From));
        }

        const FEntity Moved = Dense[static_cast<int32>(From)];
        const FEntity Other = Dense[static_cast<int32>(To)];
        Dense[static_cast<int32>(To)] = Moved;
        Dense[static_cast<int32>(From)] = Other;
        AssureSlot(Moved.GetIndex()) = FEntity(To, Moved.GetVersion());
        AssureSlot(Other.GetIndex()) = FEntity(From, Other.GetVersion());
        if (bTrackChanges)
        {
            ChangeTicks.Swap(static_cast<int32>(From), static_cast<int32>(To));
            AddedTicks.Swap(static_cast<int32>(From), static_cast<int32>(To));
        }
        BumpLayoutEpoch();
    }

    uint32 FSparseSet::JoinOwningGroup(FEntity Entity, uint32 DenseIndex)
    {
        FOwningGroup& Group = *OwningGroup;
        for (const FSparseSet* Set : Group.Owned)
        {
            if (Set != this && !Set->Contains(Entity))
            {
                return DenseIndex;
            }
        }

        const uint32 Target = static_cast<uint32>(Group.Num++);
        for (FSparseSet* Set : Group.Owned)
        {
            if (Set != this)
            {
                Set->SwapDenseSlots(Set->GetDenseIndex(Entity), Target);
            }
        }
        if (Target == DenseIndex)
        {
            return DenseIndex;
        }
        MoveDenseSlot(Target, DenseIndex);
        return Target;
    }

    void FSparseSet::LeaveOwningGroup(FEntity Entity)
    {
        FOwningGroup& Group = *OwningGroup;
        if (static_cast<int32>(GetDenseIndex(Entity)) >= Group.Num)
        {
            return;
        }

        const uint32 Last = static_cast<uint32>(--Group.Num);
        for (FSparseSet* Set : Group.Owned)
        {
            Set->SwapDenseSlots(Set->GetDenseIndex(Entity), Last);
        }
    }

    uint32 FSparseSet::ReleaseSlot(FEntity Entity)
    {
        const uint32 EntityIndex = Entity.GetIndex();
        const FEntity Slot = FindSlot(EntityIndex);
        if (Entity.IsNull() || Slot.GetVersion() != Entity.GetVersion())
        {
            return InvalidDenseIndex;
        }

        AssureSlot(EntityIndex) = NullEntity;
        --LiveCount;
        return Slot.GetIndex();
    }

    bool FSparseSet::RemoveEntity(FEntity Entity)
    {
        if (OwningGroup != nullptr && Contains(Entity))
        {
            LeaveOwningGroup(Entity);
        }

        const uint32 DenseIndex = ReleaseSlot(Entity);
        if (DenseIndex == InvalidDenseIndex)
        {
            return false;
        }

        BumpLayoutEpoch();
        BumpWriteEpoch();

        // Only a stable pool leaves a hole. A paged pool that owes nobody a fixed address still swaps.
        if (bInPlaceDelete)
        {
            if (ElementSize != 0 && !TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(TypeInfo, GetRawAtDense(DenseIndex));
            }

            Dense[DenseIndex] = MakeTombstone(FreeDenseHead);
            FreeDenseHead = DenseIndex;
            return true;
        }

        const uint32 LastIndex = static_cast<uint32>(Dense.Num()) - 1u;

        if (ElementSize != 0)
        {
            void* Target = GetRawAtDense(DenseIndex);

            if (!TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(TypeInfo, Target);
            }

            if (DenseIndex != LastIndex)
            {
                void* Source = GetRawAtDense(LastIndex);
                if (TypeInfo.Relocate != nullptr)
                {
                    TypeInfo.Relocate(TypeInfo, Target, Source);
                }
                else
                {
                    Memory::Memcpy(Target, Source, ElementSize);
                }
            }
        }

        if (DenseIndex != LastIndex)
        {
            const FEntity Moved = Dense[LastIndex];
            Dense[DenseIndex] = Moved;
            AssureSlot(Moved.GetIndex()) = FEntity(DenseIndex, Moved.GetVersion());
            if (bTrackChanges)
            {
                ChangeTicks[static_cast<int32>(DenseIndex)] = ChangeTicks[static_cast<int32>(LastIndex)];
                AddedTicks[static_cast<int32>(DenseIndex)] = AddedTicks[static_cast<int32>(LastIndex)];
            }
        }

        Dense.Pop();
        if (bTrackChanges)
        {
            ChangeTicks.Pop();
            AddedTicks.Pop();
        }
        return true;
    }

    void* FSparseSet::EmplaceDefaultRaw(FEntity Entity)
    {
        if (Entity.IsNull())
        {
            return nullptr;
        }

        const FEntity ExistingSlot = FindSlot(Entity.GetIndex());
        if (ExistingSlot.GetVersion() == Entity.GetVersion())
        {
            if (ElementSize == 0)
            {
                MarkWritten(ExistingSlot.GetIndex());
                return nullptr;
            }

            void* Target = GetRawAtDense(ExistingSlot.GetIndex());
            if (!TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(TypeInfo, Target);
            }
            TypeInfo.DefaultConstruct(TypeInfo, Target);
            MarkWritten(ExistingSlot.GetIndex());
            return Target;
        }

        const uint32 DenseIndex = AllocateSlot(Entity);
        if (ElementSize == 0)
        {
            return nullptr;
        }

        void* Target = GetRawAtDense(DenseIndex);
        TypeInfo.DefaultConstruct(TypeInfo, Target);
        return Target;
    }

    void* FSparseSet::EmplaceCopyRaw(FEntity Entity, const void* Source)
    {
        if (Source == nullptr || ElementSize == 0 || Entity.IsNull())
        {
            return EmplaceDefaultRaw(Entity);
        }

        const FEntity ExistingSlot = FindSlot(Entity.GetIndex());
        if (ExistingSlot.GetVersion() == Entity.GetVersion())
        {
            void* Target = GetRawAtDense(ExistingSlot.GetIndex());
            if (!TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(TypeInfo, Target);
            }
            TypeInfo.CopyConstruct(TypeInfo, Target, Source);
            MarkWritten(ExistingSlot.GetIndex());
            return Target;
        }

        const uint32 DenseIndex = AllocateSlot(Entity);
        void* Target = GetRawAtDense(DenseIndex);
        TypeInfo.CopyConstruct(TypeInfo, Target, Source);
        return Target;
    }

    void FSparseSet::ClearAll()
    {
        BumpLayoutEpoch();
        BumpWriteEpoch();
        DestroyLiveElements();

        for (FEntity* Page : SparsePages)
        {
            if (Page != nullptr)
            {
                Memory::Memset(Page, 0xFF, sizeof(FEntity) * SparsePageSize);
            }
        }

        Dense.Reset();
        ChangeTicks.Reset();
        AddedTicks.Reset();
        LiveCount = 0;
        FreeDenseHead = InvalidDenseIndex;
        if (OwningGroup != nullptr)
        {
            OwningGroup->Num = 0;
        }
    }

    void FSparseSet::Compact()
    {
        // A type that can neither be moved nor bit-copied keeps its tombstones, since closing the gaps would move it.
        if (!bInPlaceDelete || !HasTombstones() || (!TypeInfo.bTriviallyRelocatable && TypeInfo.Relocate == nullptr))
        {
            return;
        }

        BumpLayoutEpoch();
        BumpWriteEpoch();

        int32 Write = 0;
        for (int32 Read = 0; Read < Dense.Num(); ++Read)
        {
            if (Dense[Read].IsTombstone())
            {
                continue;
            }

            if (Write != Read)
            {
                Dense[Write] = Dense[Read];
                if (bTrackChanges)
                {
                    ChangeTicks[Write] = ChangeTicks[Read];
                    AddedTicks[Write] = AddedTicks[Read];
                }

                if (ElementSize != 0)
                {
                    void* Target = GetRawAtDense(Write);
                    void* Source = GetRawAtDense(Read);
                    if (TypeInfo.Relocate != nullptr)
                    {
                        TypeInfo.Relocate(TypeInfo, Target, Source);
                    }
                    else
                    {
                        Memory::Memcpy(Target, Source, ElementSize);
                    }
                }
            }
            ++Write;
        }

        Dense.SetNum(Write);
        if (bTrackChanges)
        {
            ChangeTicks.SetNum(Write);
            AddedTicks.SetNum(Write);
        }

        for (FEntity* Page : SparsePages)
        {
            if (Page != nullptr)
            {
                Memory::Memset(Page, 0xFF, sizeof(FEntity) * SparsePageSize);
            }
        }

        for (int32 Index = 0; Index < Dense.Num(); ++Index)
        {
            AssureSlot(Dense[Index].GetIndex()) = FEntity(Index, Dense[Index].GetVersion());
        }

        FreeDenseHead = InvalidDenseIndex;
        LiveCount = Dense.Num();
    }
}
