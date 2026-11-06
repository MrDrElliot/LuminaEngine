#pragma once

#include "Entity.h"
#include "Containers/Vector.h"
#include "Lumina.h"

namespace Lumina::ECS
{
    // A value per entity, looked up through paged entity-index slots like a component pool instead of by hashing.
    template<typename T>
    class TEntityMap
    {
    public:

        static constexpr uint32 PageSize = 4096;

        NODISCARD FORCEINLINE T* Find(FEntity Entity)
        {
            const uint32 Dense = FindDense(Entity);
            return Dense != InvalidDense ? &Values[Dense] : nullptr;
        }

        NODISCARD FORCEINLINE const T* Find(FEntity Entity) const
        {
            const uint32 Dense = FindDense(Entity);
            return Dense != InvalidDense ? &Values[Dense] : nullptr;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto FindChecked(this TSelf&& Self, FEntity Entity) -> TLikeSelf<TSelf, T>&
        {
            auto* Found = Self.Find(Entity);
            LUMINA_CONTAINER_CHECK(Found != nullptr);
            return *Found;
        }

        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const { return FindDense(Entity) != InvalidDense; }

        // Default-constructs the value when the entity has none yet.
        T& FindOrAdd(FEntity Entity)
        {
            if (T* Existing = Find(Entity))
            {
                return *Existing;
            }

            // A destroyed entity whose index was reused still owns a value here; dropping it keeps one entry per index.
            FEntity& Slot = AssureSlot(Entity.GetIndex());
            if (!Slot.IsNull())
            {
                RemoveAt(Slot.GetIndex());
            }

            const uint32 Dense = (uint32)Keys.Num();
            Slot = FEntity(Dense, Entity.GetVersion());
            Keys.Add(Entity);
            return Values.EmplaceGetRef();
        }

        // Swap-removes, so it moves the last value and invalidates pointers to it.
        bool Remove(FEntity Entity)
        {
            const uint32 Dense = FindDense(Entity);
            if (Dense == InvalidDense)
            {
                return false;
            }
            RemoveAt(Dense);
            return true;
        }

        // Removes by dense position, so a sweep over GetKeyAt can drop entries without looking them up again.
        void RemoveAt(uint32 Dense)
        {
            const FEntity Removed = Keys[Dense];
            const uint32  Last    = (uint32)Keys.Num() - 1u;
            if (Dense != Last)
            {
                const FEntity Moved = Keys[Last];
                Keys[Dense] = Moved;
                Values[Dense] = Move(Values[Last]);
                Pages[Moved.GetIndex() / PageSize][Moved.GetIndex() % PageSize] = FEntity(Dense, Moved.GetVersion());
            }
            Pages[Removed.GetIndex() / PageSize][Removed.GetIndex() % PageSize] = NullEntity;
            Keys.Pop();
            Values.Pop();
        }

        void Clear()
        {
            Pages.Reset();
            Keys.Reset();
            Values.Reset();
        }

        NODISCARD FORCEINLINE uint32 Num() const { return (uint32)Keys.Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const { return Keys.IsEmpty(); }

        NODISCARD FORCEINLINE FEntity GetKeyAt(uint32 Dense) const { return Keys[Dense]; }
        NODISCARD FORCEINLINE T& GetValueAt(uint32 Dense) { return Values[Dense]; }

    private:

        static constexpr uint32 InvalidDense = Constants::kIndexNoneU32;

        // A real entity never carries the null handle's version, so an empty slot fails the version test.
        NODISCARD FORCEINLINE uint32 FindDense(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            const uint32 Page = Index / PageSize;
            if (Entity.IsNull() || Page >= (uint32)Pages.Num() || Pages[Page].IsEmpty())
            {
                return InvalidDense;
            }
            const FEntity Slot = Pages[Page][Index % PageSize];
            return Slot.GetVersion() == Entity.GetVersion() ? Slot.GetIndex() : InvalidDense;
        }

        FEntity& AssureSlot(uint32 Index)
        {
            const uint32 Page = Index / PageSize;
            if (Page >= (uint32)Pages.Num())
            {
                Pages.SetNum(Page + 1u);
            }
            if (Pages[Page].IsEmpty())
            {
                Pages[Page].SetNum(PageSize, NullEntity);
            }
            return Pages[Page][Index % PageSize];
        }

        TVector<TVector<FEntity>> Pages;
        TVector<FEntity> Keys;
        TVector<T> Values;
    };
}
