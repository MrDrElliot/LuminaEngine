#include "RuntimePCH.h"
#include "SparseSet.h"

#include "Core/Assertions/Assert.h"

namespace Lumina::ECS
{
    namespace
    {
        constexpr size_t MinPackedCapacity = 8;

        // A payload block under-aligned to its element straddles cache lines on every other store.
        constexpr size_t MinPayloadAlignment = 16;

        NODISCARD constexpr size_t PayloadAlignment(uint32 ElementAlignment)
        {
            return ElementAlignment > MinPayloadAlignment ? ElementAlignment : MinPayloadAlignment;
        }
    }

    FSparseSet::FSparseSet(const FComponentTypeInfo& InTypeInfo)
        : TypeInfo(InTypeInfo)
        , ElementSize(InTypeInfo.bEmpty ? 0u : InTypeInfo.Size)
        , ElementAlignment(InTypeInfo.Alignment)
        , bPaged(InTypeInfo.bPaged && !InTypeInfo.bEmpty)
        , bInPlaceDelete(InTypeInfo.bInPlaceDelete)
    {
        const uint32 RequestedPageSize = InTypeInfo.PageSize != 0u ? InTypeInfo.PageSize : 1024u;
        DEBUG_ASSERT((RequestedPageSize & (RequestedPageSize - 1u)) == 0u, "page size must be a power of two");

        while ((1u << PayloadPageShift) < RequestedPageSize)
        {
            ++PayloadPageShift;
        }

        PayloadPageMask = RequestedPageSize - 1u;
    }

    FSparseSet::~FSparseSet()
    {
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
        PayloadPages.clear();
    }

    void FSparseSet::DestroyLiveElements()
    {
        if (ElementSize == 0 || TypeInfo.bTriviallyDestructible || TypeInfo.Destruct == nullptr)
        {
            return;
        }

        for (uint32 Index = 0; Index < static_cast<uint32>(Dense.size()); ++Index)
        {
            if (!Dense[Index].IsTombstone())
            {
                TypeInfo.Destruct(GetRawAtDense(Index));
            }
        }
    }

    FEntity& FSparseSet::AssureSlot(uint32 EntityIndex)
    {
        const uint32 Page = EntityIndex / SparsePageSize;
        if (Page >= SparsePages.size())
        {
            SparsePages.resize(Page + 1u, nullptr);
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

    void FSparseSet::GrowPayloadTo(size_t ElementCount)
    {
        if (ElementSize == 0)
        {
            return;
        }

        if (bPaged)
        {
            const size_t PageSize = static_cast<size_t>(PayloadPageMask) + 1u;
            const size_t NeededPages = (ElementCount + PageSize - 1u) / PageSize;
            while (PayloadPages.size() < NeededPages)
            {
                PayloadPages.push_back(static_cast<uint8*>(
                    Memory::Malloc(static_cast<size_t>(ElementSize) * PageSize, PayloadAlignment(ElementAlignment))));
            }
            return;
        }

        if (ElementCount <= PackedCapacity)
        {
            return;
        }

        size_t NewCapacity = PackedCapacity < MinPackedCapacity ? MinPackedCapacity : PackedCapacity;
        while (NewCapacity < ElementCount)
        {
            NewCapacity *= 2u;
        }

        // Realloc first, because rpmalloc can often widen the block in place and skip the copy entirely.
        if (TypeInfo.bTriviallyRelocatable || TypeInfo.Relocate == nullptr)
        {
            PackedData = static_cast<uint8*>(
                Memory::Realloc(PackedData, NewCapacity * ElementSize, PayloadAlignment(ElementAlignment)));
        }
        else
        {
            uint8* NewBlock = static_cast<uint8*>(Memory::Malloc(NewCapacity * ElementSize, PayloadAlignment(ElementAlignment)));

            // Dense.size() is the constructed count, which is why growth has to happen before a push.
            for (size_t Index = 0; Index < Dense.size(); ++Index)
            {
                TypeInfo.Relocate(NewBlock + Index * ElementSize, PackedData + Index * ElementSize);
            }
            if (PackedData != nullptr)
            {
                void* Block = PackedData;
                Memory::Free(Block);
            }
            PackedData = NewBlock;
        }

        PackedCapacity = NewCapacity;
    }

    void FSparseSet::Reserve(size_t Count)
    {
        Dense.reserve(Count);
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
        }
        else
        {
            DenseIndex = static_cast<uint32>(Dense.size());
            ASSERT(DenseIndex <= MaxDenseIndex);

            // Grows before the push, so the relocation loop only ever sees constructed elements.
            GrowPayloadTo(static_cast<size_t>(DenseIndex) + 1u);
            Dense.push_back(Entity);
        }

        Slot = FEntity(DenseIndex, Entity.GetVersion());
        ++LiveCount;
        return DenseIndex;
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
        const uint32 DenseIndex = ReleaseSlot(Entity);
        if (DenseIndex == InvalidDenseIndex)
        {
            return false;
        }

        // Only a stable pool leaves a hole. A paged pool that owes nobody a fixed address still swaps.
        if (bInPlaceDelete)
        {
            if (ElementSize != 0 && !TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(GetRawAtDense(DenseIndex));
            }

            Dense[DenseIndex] = MakeTombstone(FreeDenseHead);
            FreeDenseHead = DenseIndex;
            return true;
        }

        const uint32 LastIndex = static_cast<uint32>(Dense.size()) - 1u;

        if (ElementSize != 0)
        {
            void* Target = GetRawAtDense(DenseIndex);

            if (!TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(Target);
            }

            if (DenseIndex != LastIndex)
            {
                void* Source = GetRawAtDense(LastIndex);
                if (TypeInfo.Relocate != nullptr)
                {
                    TypeInfo.Relocate(Target, Source);
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
        }

        Dense.pop_back();
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
                return nullptr;
            }

            void* Target = GetRawAtDense(ExistingSlot.GetIndex());
            if (!TypeInfo.bTriviallyDestructible && TypeInfo.Destruct != nullptr)
            {
                TypeInfo.Destruct(Target);
            }
            TypeInfo.DefaultConstruct(Target);
            return Target;
        }

        const uint32 DenseIndex = AllocateSlot(Entity);
        if (ElementSize == 0)
        {
            return nullptr;
        }

        void* Target = GetRawAtDense(DenseIndex);
        TypeInfo.DefaultConstruct(Target);
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
                TypeInfo.Destruct(Target);
            }
            TypeInfo.CopyConstruct(Target, Source);
            return Target;
        }

        const uint32 DenseIndex = AllocateSlot(Entity);
        void* Target = GetRawAtDense(DenseIndex);
        TypeInfo.CopyConstruct(Target, Source);
        return Target;
    }

    void FSparseSet::ClearAll()
    {
        DestroyLiveElements();

        for (FEntity* Page : SparsePages)
        {
            if (Page != nullptr)
            {
                Memory::Memset(Page, 0xFF, sizeof(FEntity) * SparsePageSize);
            }
        }

        Dense.clear();
        LiveCount = 0;
        FreeDenseHead = InvalidDenseIndex;
    }

    void FSparseSet::Compact()
    {
        if (!bInPlaceDelete || !HasTombstones())
        {
            return;
        }

        uint32 Write = 0;
        for (uint32 Read = 0; Read < static_cast<uint32>(Dense.size()); ++Read)
        {
            if (Dense[Read].IsTombstone())
            {
                continue;
            }

            if (Write != Read)
            {
                Dense[Write] = Dense[Read];

                if (ElementSize != 0)
                {
                    void* Target = GetRawAtDense(Write);
                    void* Source = GetRawAtDense(Read);
                    if (TypeInfo.Relocate != nullptr)
                    {
                        TypeInfo.Relocate(Target, Source);
                    }
                    else
                    {
                        Memory::Memcpy(Target, Source, ElementSize);
                    }
                }
            }
            ++Write;
        }

        Dense.resize(Write);

        for (FEntity* Page : SparsePages)
        {
            if (Page != nullptr)
            {
                Memory::Memset(Page, 0xFF, sizeof(FEntity) * SparsePageSize);
            }
        }

        for (uint32 Index = 0; Index < static_cast<uint32>(Dense.size()); ++Index)
        {
            AssureSlot(Dense[Index].GetIndex()) = FEntity(Index, Dense[Index].GetVersion());
        }

        FreeDenseHead = InvalidDenseIndex;
        LiveCount = Dense.size();
    }
}
