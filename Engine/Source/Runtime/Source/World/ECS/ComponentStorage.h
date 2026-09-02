#pragma once

#include "SparseSet.h"
#include "Memory/Construct.h"
#include "Containers/Tuple.h"
#include "Core/Assertions/Assert.h"

#include <new>
#include <utility>

namespace Lumina::ECS
{
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
        static constexpr size_t PageSize = TComponentTraits<T>::PageSize;

        // A tag has no value, so Emplace hands back nothing rather than a reference to a shared dummy.
        using FEmplaceResult = std::conditional_t<bEmpty, void, T&>;

        TComponentStorage() = default;
        explicit TComponentStorage(FSparseSet* InSet) : Set(InSet) {}

        NODISCARD FORCEINLINE bool IsValid() const { return Set != nullptr; }
        NODISCARD FORCEINLINE FSparseSet* GetSet() const { return Set; }

        NODISCARD FORCEINLINE const TComponentStorage* operator -> () const { return this; }
        NODISCARD FORCEINLINE explicit operator bool() const { return Set != nullptr; }

        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const { return Set->Contains(Entity); }
        NODISCARD FORCEINLINE size_t Num() const { return Set->Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const { return Set->IsEmpty(); }
        NODISCARD FORCEINLINE bool HasTombstones() const { return Set->HasTombstones(); }
        NODISCARD FORCEINLINE size_t GetDenseSize() const { return Set->GetDenseSize(); }
        NODISCARD FORCEINLINE const FEntity* GetDenseData() const { return Set->GetDenseData(); }

        // Tuple iteration over the pool, matching the shape a view hands back.
        class FEachIterator
        {
        public:

            FEachIterator() = default;

            FEachIterator(const TComponentStorage* InStorage, size_t InIndex)
                : Storage(InStorage)
                , Index(InIndex)
            {
                Advance();
            }

            NODISCARD auto operator * () const
            {
                return TTuple<FEntity, T&>(Storage->GetDenseData()[Index],
                    Storage->GetAtDense(static_cast<uint32>(Index)));
            }

            FEachIterator& operator ++ () { ++Index; Advance(); return *this; }

            NODISCARD bool operator == (const FEachIterator& Other) const { return Index == Other.Index; }
            NODISCARD bool operator != (const FEachIterator& Other) const { return Index != Other.Index; }

        private:

            void Advance()
            {
                if (Storage == nullptr || !Storage->IsValid())
                {
                    return;
                }

                const size_t Count = Storage->GetDenseSize();
                const FEntity* Dense = Storage->GetDenseData();

                while (Index < Count && Dense[Index].IsTombstone())
                {
                    ++Index;
                }
            }

            const TComponentStorage* Storage = nullptr;
            size_t Index = 0;
        };

        struct FEachRange
        {
            const TComponentStorage* Storage = nullptr;

            NODISCARD FEachIterator begin() const { return FEachIterator(Storage, 0); }
            NODISCARD FEachIterator end() const { return FEachIterator(Storage, Storage->GetDenseSize()); }
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

        // Undefined unless the pool holds the entity, so the check is worth its cost outside shipping.
        NODISCARD FORCEINLINE T& Get(FEntity Entity) const requires CDataComponent<T>
        {
            DEBUG_ASSERT(Set->Contains(Entity), "Get on a component the entity does not have");
            return GetAtDense(Set->GetDenseIndex(Entity));
        }

        // Does the lookup itself rather than through GetRaw, so the layout branches fold away at compile time.
        NODISCARD FORCEINLINE T* TryGet(FEntity Entity) const requires CDataComponent<T>
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
            const uint32 DenseIndex = Set->AllocateSlot(Entity);
            if constexpr (!bEmpty)
            {
                Memory::ConstructAt(&GetAtDense(DenseIndex), std::forward<TArgs>(Args)...);
                return GetAtDense(DenseIndex);
            }
        }

        template<typename... TArgs>
        FEmplaceResult EmplaceOrReplace(FEntity Entity, TArgs&&... Args) const
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            if (Set->Contains(Entity))
            {
                if constexpr (!bEmpty)
                {
                    T& Existing = Get(Entity);
                    Memory::DestroyAt(&Existing);
                    Memory::ConstructAt(&Existing, std::forward<TArgs>(Args)...);
                    return Existing;
                }
                else
                {
                    return;
                }
            }
            return Emplace(Entity, std::forward<TArgs>(Args)...);
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
            return Emplace(Entity, std::forward<TArgs>(Args)...);
        }

        bool RemoveEntity(FEntity Entity) const { return Set->RemoveEntity(Entity); }

        void Reserve(size_t Count) const { Set->Reserve(Count); }

        // Both callables by value, or the compiler must assume a store in the body aliases them.
        template<typename TPredicate, typename TFunc>
        FORCEINLINE void ForEachDenseWhere(TPredicate Accept, TFunc Func) const
        {
            const FEntity* DenseData = Set->GetDenseData();
            const size_t DenseSize = Set->GetDenseSize();

            // A tag reaches the callback as membership only, so the value is never in the argument list.
            if constexpr (bEmpty)
            {
                for (size_t Index = 0; Index < DenseSize; ++Index)
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
                const size_t PageCount = (DenseSize + PageSize - 1u) / PageSize;

                for (size_t PageIndex = 0; PageIndex < PageCount; ++PageIndex)
                {
                    T* Page = reinterpret_cast<T*>(Set->GetPayloadPage(PageIndex));
                    const size_t Base = PageIndex * PageSize;
                    const size_t Count = (Base + PageSize <= DenseSize) ? PageSize : (DenseSize - Base);

                    for (size_t Offset = 0; Offset < Count; ++Offset)
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
                for (size_t Index = 0; Index < DenseSize; ++Index)
                {
                    const FEntity Entity = DenseData[Index];
                    if (Accept(Entity))
                    {
                        InvokeEntityCallback(Func, Entity, Elements[Index]);
                    }
                }
            }
        }

        // Storage order, so the page pointer advances once per page rather than once per element.
        template<typename TFunc>
        FORCEINLINE void ForEachDense(TFunc Func) const
        {
            const FEntity* DenseData = Set->GetDenseData();
            const size_t DenseSize = Set->GetDenseSize();

            if constexpr (bEmpty)
            {
                for (size_t Index = 0; Index < DenseSize; ++Index)
                {
                    Func(DenseData[Index]);
                }
            }
            else if constexpr (bPaged)
            {
                const bool bSkipHoles = bInPlaceDelete && Set->HasTombstones();
                const size_t PageCount = (DenseSize + PageSize - 1u) / PageSize;

                for (size_t PageIndex = 0; PageIndex < PageCount; ++PageIndex)
                {
                    T* Page = reinterpret_cast<T*>(Set->GetPayloadPage(PageIndex));
                    const size_t Base = PageIndex * PageSize;
                    const size_t Count = (Base + PageSize <= DenseSize) ? PageSize : (DenseSize - Base);

                    // Split rather than branching per element, so the common no-hole page stays a tight loop.
                    if (bSkipHoles)
                    {
                        for (size_t Offset = 0; Offset < Count; ++Offset)
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
                        for (size_t Offset = 0; Offset < Count; ++Offset)
                        {
                            InvokeEntityCallback(Func, DenseData[Base + Offset], Page[Offset]);
                        }
                    }
                }
            }
            else
            {
                T* Elements = reinterpret_cast<T*>(Set->GetPackedBlock());
                for (size_t Index = 0; Index < DenseSize; ++Index)
                {
                    InvokeEntityCallback(Func, DenseData[Index], Elements[Index]);
                }
            }
        }

    private:

        FSparseSet* Set = nullptr;
    };

    // The lens claim, checked rather than asserted in a comment.
    static_assert(sizeof(TComponentStorage<FEntity>) == sizeof(void*), "a typed storage is one pointer.");
    static_assert(std::is_trivially_copyable_v<TComponentStorage<FEntity>>, "a typed storage is passed by value.");
}
