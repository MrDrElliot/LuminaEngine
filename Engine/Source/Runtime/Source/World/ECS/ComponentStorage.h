#pragma once

#include "SparseSet.h"
#include "Memory/Construct.h"
#include "Containers/Tuple.h"
#include "Core/Assertions/Assert.h"

#include <new>
#include <utility>

namespace Lumina::ECS
{
    // Removal never reallocates, so moved arrays mean the walk's own callback added to the pool it was reading.
    FORCEINLINE void CheckWalkDidNotReallocate([[maybe_unused]] const FSparseSet* Set,
        [[maybe_unused]] const FEntity* DenseData, [[maybe_unused]] const uint8* PackedBlock)
    {
        DEBUG_ASSERT(Set->GetDenseData() == DenseData && Set->GetPackedBlock() == PackedBlock,
            "A component was added to a pool while iterating it, which reallocated the arrays the walk was reading");
    }

    // A typed lens onto one FSparseSet. Holds a pointer and nothing else, so it costs the same to pass as one.
    template<CComponent T>
    class TComponentStorage
    {
    public:

        using ValueType = T;

        static constexpr bool bEmpty = TComponentTraits<T>::bEmpty;
        static constexpr bool bPaged = TComponentTraits<T>::bPaged;

        // Compile-time here, so the typed hot path folds page math that the type-erased side has to compute.
        static constexpr bool bInPlaceDelete = TComponentTraits<T>::InPlaceDelete;
        static constexpr int32 PageSize = TComponentTraits<T>::PageSize;

        // A tag has no value, so Emplace hands back nothing rather than a reference to a shared dummy.
        using FEmplaceResult = std::conditional_t<bEmpty, void, T&>;

        TComponentStorage() = default;
        explicit TComponentStorage(FSparseSet* InSet) : Set(InSet) {}

        NODISCARD FORCEINLINE bool IsValid() const { return Set != nullptr; }
        NODISCARD FORCEINLINE FSparseSet* GetSet() const { return Set; }

        NODISCARD FORCEINLINE const TComponentStorage* operator -> () const { return this; }
        NODISCARD FORCEINLINE explicit operator bool() const { return Set != nullptr; }

        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const { return Set->Contains(Entity); }
        NODISCARD FORCEINLINE int32 Num() const { return Set->Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const { return Set->IsEmpty(); }

        // Safe on a storage FindStorage found nothing for, which IsEmpty is not.
        NODISCARD FORCEINLINE bool IsEmptyOrMissing() const { return Set == nullptr || Set->IsEmpty(); }
        NODISCARD FORCEINLINE bool HasTombstones() const { return Set->HasTombstones(); }
        NODISCARD FORCEINLINE int32 GetDenseSize() const { return Set->GetDenseSize(); }
        NODISCARD FORCEINLINE const FEntity* GetDenseData() const { return Set->GetDenseData(); }

        // Tuple iteration over the pool, matching the shape a view hands back. Walks backward like every pool walk.
        class FEachIterator
        {
        public:

            FEachIterator() = default;

            FEachIterator(const TComponentStorage* InStorage, int32 InEndSlot)
                : Storage(InStorage)
                , EndSlot(InEndSlot)
            {
                Advance();
            }

            NODISCARD auto operator * () const
            {
                const int32 Index = EndSlot - 1;
                Storage->GetSet()->StampIfTracked(static_cast<uint32>(Index));
                return TTuple<FEntity, T&>(Storage->GetDenseData()[Index],
                    Storage->GetAtDense(static_cast<uint32>(Index)));
            }

            FEachIterator& operator ++ () { --EndSlot; Advance(); return *this; }

            NODISCARD bool operator == (const FEachIterator& Other) const { return EndSlot == Other.EndSlot; }
            NODISCARD bool operator != (const FEachIterator& Other) const { return EndSlot != Other.EndSlot; }

        private:

            void Advance()
            {
                if (Storage == nullptr || !Storage->IsValid())
                {
                    return;
                }

                const FEntity* Dense = Storage->GetDenseData();
                while (EndSlot > 0 && Dense[EndSlot - 1].IsTombstone())
                {
                    --EndSlot;
                }
            }

            const TComponentStorage* Storage = nullptr;

            // One past the current slot, so the finished walk sits at zero without wrapping.
            int32 EndSlot = 0;
        };

        struct FEachRange
        {
            const TComponentStorage* Storage = nullptr;

            NODISCARD FEachIterator begin() const { return FEachIterator(Storage, Storage->GetDenseSize()); }
            NODISCARD FEachIterator end() const { return FEachIterator(Storage, 0); }
        };

        NODISCARD FEachRange Each() const requires CDataComponent<T> { return FEachRange{ this }; }

        // sizeof(T) is known here, so this compiles to the same address math a hand-written array would.
        NODISCARD FORCEINLINE T& GetAtDense(uint32 DenseIndex) const requires CDataComponent<T>
        {
            if constexpr (bPaged)
            {
                return reinterpret_cast<T*>(Set->GetPayloadPage(DenseIndex / PageSize))
                    [DenseIndex % PageSize];
            }
            else
            {
                return reinterpret_cast<T*>(Set->GetPackedBlock())[DenseIndex];
            }
        }

        // Undefined unless the pool holds the entity. Mutable access, so a tracked pool stamps the element.
        NODISCARD FORCEINLINE T& Get(FEntity Entity) const requires CDataComponent<T>
        {
            DEBUG_ASSERT(Set->Contains(Entity), "Get on a component the entity does not have");
            const uint32 DenseIndex = Set->GetDenseIndex(Entity);
            Set->StampIfTracked(DenseIndex);
            return GetAtDense(DenseIndex);
        }

        NODISCARD FORCEINLINE T* TryGet(FEntity Entity) const requires CDataComponent<T>
        {
            T* Value = TryGetUntracked(Entity);
            if (Value != nullptr)
            {
                Set->StampIfTracked(Set->GetDenseIndex(Entity));
            }
            return Value;
        }

        // Never stamps, for a reader or for a caller that stamps by itself.
        NODISCARD FORCEINLINE T& GetUntracked(FEntity Entity) const requires CDataComponent<T>
        {
            DEBUG_ASSERT(Set->Contains(Entity), "Get on a component the entity does not have");
            return GetAtDense(Set->GetDenseIndex(Entity));
        }

        // Does the lookup itself rather than through GetRaw, so the layout branches fold away at compile time.
        NODISCARD FORCEINLINE T* TryGetUntracked(FEntity Entity) const requires CDataComponent<T>
        {
            // An empty slot is memset to the null handle, so a null query would match one and index out of bounds.
            if (Entity.IsNull())
            {
                return nullptr;
            }
            const FEntity Slot = Set->FindSlot(Entity.GetIndex());
            if (Slot.GetVersion() != Entity.GetVersion())
            {
                return nullptr;
            }
            return &GetAtDense(Slot.GetIndex());
        }

        // A tag takes no constructor arguments, because there is nothing to construct.
        template<typename... TArgs>
        FEmplaceResult Emplace(FEntity Entity, TArgs&&... Args) const
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            static_assert(std::is_trivially_copyable_v<T> || TIsTriviallyRelocatable_V<T> || std::is_move_constructible_v<T> || TComponentTraits<T>::InPlaceDelete,
                "A pool moves its components on removal, so one without a move constructor must be trivially relocatable or in-place-delete.");

            const uint32 DenseIndex = Set->AllocateSlot(Entity);
            if constexpr (!bEmpty)
            {
                Memory::ConstructAt(&GetAtDense(DenseIndex), Forward<TArgs>(Args)...);
                return GetAtDense(DenseIndex);
            }
        }

        template<typename... TArgs>
        FEmplaceResult EmplaceOrReplace(FEntity Entity, TArgs&&... Args) const
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            if (Set->Contains(Entity))
            {
                const uint32 DenseIndex = Set->GetDenseIndex(Entity);
                Set->MarkWritten(DenseIndex);
                if constexpr (!bEmpty)
                {
                    T& Existing = GetAtDense(DenseIndex);
                    Memory::DestroyAt(&Existing);
                    Memory::ConstructAt(&Existing, Forward<TArgs>(Args)...);
                    return Existing;
                }
                else
                {
                    return;
                }
            }
            return Emplace(Entity, Forward<TArgs>(Args)...);
        }

        template<typename... TArgs>
        FEmplaceResult GetOrEmplace(FEntity Entity, TArgs&&... Args) const
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            if (Set->Contains(Entity))
            {
                if constexpr (!bEmpty)
                {
                    return Get(Entity);
                }
                else
                {
                    return;
                }
            }
            return Emplace(Entity, Forward<TArgs>(Args)...);
        }

        bool RemoveEntity(FEntity Entity) const { return Set->RemoveEntity(Entity); }

        void Reserve(int32 Count) const { Set->Reserve(Count); }

        // Both callables by value, or the compiler must assume a store in the body aliases them.
        template<typename TPredicate, typename TFunc>
        FORCEINLINE void ForEachDenseWhere(TPredicate Accept, TFunc Func) const
        {
            const FEntity* DenseData = Set->GetDenseData();
            const int32 DenseSize = Set->GetDenseSize();
            const uint8* PackedBlock = Set->GetPackedBlock();

            // A tag reaches the callback as membership only, so the value is never in the argument list.
            if constexpr (bEmpty)
            {
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    const FEntity Entity = DenseData[Index];
                    if (Accept(Entity))
                    {
                        Func(Entity);
                    }
                }
            }
            else if constexpr (bPaged)
            {
                const bool bSkipHoles = bInPlaceDelete && Set->HasTombstones();
                const int32 PageCount = (DenseSize + PageSize - 1) / PageSize;

                for (int32 PageIndex = PageCount; PageIndex-- > 0;)
                {
                    T* Page = reinterpret_cast<T*>(Set->GetPayloadPage(PageIndex));
                    const int32 Base = PageIndex * PageSize;
                    const int32 Count = (Base + PageSize <= DenseSize) ? PageSize : (DenseSize - Base);

                    for (int32 Offset = Count; Offset-- > 0;)
                    {
                        const FEntity Entity = DenseData[Base + Offset];
                        if ((!bSkipHoles || !Entity.IsTombstone()) && Accept(Entity))
                        {
                            InvokeEntityCallback(Func, Entity, Page[Offset]);
                        }
                    }
                }
            }
            else
            {
                T* Elements = reinterpret_cast<T*>(Set->GetPackedBlock());
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    const FEntity Entity = DenseData[Index];
                    if (Accept(Entity))
                    {
                        InvokeEntityCallback(Func, Entity, Elements[Index]);
                    }
                }
            }

            CheckWalkDidNotReallocate(Set, DenseData, PackedBlock);
        }

        // Backward, so swap-and-pop only ever pulls an already visited element into the slot being removed.
        template<typename TFunc>
        FORCEINLINE void ForEachDense(TFunc Func) const
        {
            const FEntity* DenseData = Set->GetDenseData();
            const int32 DenseSize = Set->GetDenseSize();
            const uint8* PackedBlock = Set->GetPackedBlock();

            if constexpr (bEmpty)
            {
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    Func(DenseData[Index]);
                }
            }
            else if constexpr (bPaged)
            {
                const bool bSkipHoles = bInPlaceDelete && Set->HasTombstones();
                const int32 PageCount = (DenseSize + PageSize - 1) / PageSize;

                for (int32 PageIndex = PageCount; PageIndex-- > 0;)
                {
                    T* Page = reinterpret_cast<T*>(Set->GetPayloadPage(PageIndex));
                    const int32 Base = PageIndex * PageSize;
                    const int32 Count = (Base + PageSize <= DenseSize) ? PageSize : (DenseSize - Base);

                    // Split rather than branching per element, so the common no-hole page stays a tight loop.
                    if (bSkipHoles)
                    {
                        for (int32 Offset = Count; Offset-- > 0;)
                        {
                            const FEntity Entity = DenseData[Base + Offset];
                            if (!Entity.IsTombstone())
                            {
                                InvokeEntityCallback(Func, Entity, Page[Offset]);
                            }
                        }
                    }
                    else
                    {
                        for (int32 Offset = Count; Offset-- > 0;)
                        {
                            InvokeEntityCallback(Func, DenseData[Base + Offset], Page[Offset]);
                        }
                    }
                }
            }
            else
            {
                T* Elements = reinterpret_cast<T*>(Set->GetPackedBlock());
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    InvokeEntityCallback(Func, DenseData[Index], Elements[Index]);
                }
            }

            CheckWalkDidNotReallocate(Set, DenseData, PackedBlock);
        }

    private:

        FSparseSet* Set = nullptr;
    };

    // The lens claim, checked rather than asserted in a comment.
    static_assert(sizeof(TComponentStorage<FEntity>) == sizeof(void*), "a typed storage is one pointer.");
    static_assert(std::is_trivially_copyable_v<TComponentStorage<FEntity>>, "a typed storage is passed by value.");
}
