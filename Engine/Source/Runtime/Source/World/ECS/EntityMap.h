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

        NODISCARD FORCEINLINE bool Contains(FEntity Entity) const { return FindDense(Entity) != InvalidDense; }

        // Default-constructs the value when the entity has none yet.
        T& FindOrAdd(FEntity Entity)
        {
            if (T* Existing = Find(Entity))
            {
                return *Existing;
            }
            const uint32 Dense = (uint32)Keys.size();
            AssureSlot(Entity.GetIndex()) = FEntity(Dense, Entity.GetVersion());
            Keys.push_back(Entity);
            return Values.emplace_back();
        }

        // Swap-removes, so it moves the last value and invalidates pointers to it.
        bool Remove(FEntity Entity)
        {
            const uint32 Dense = FindDense(Entity);
            if (Dense == InvalidDense)
            {
                return false;
            }
            const uint32 Last = (uint32)Keys.size() - 1u;
            if (Dense != Last)
            {
                const FEntity Moved = Keys[Last];
                Keys[Dense] = Moved;
                Values[Dense] = std::move(Values[Last]);
                Pages[Moved.GetIndex() / PageSize][Moved.GetIndex() % PageSize] = FEntity(Dense, Moved.GetVersion());
            }
            Pages[Entity.GetIndex() / PageSize][Entity.GetIndex() % PageSize] = NullEntity;
            Keys.pop_back();
            Values.pop_back();
            return true;
        }

        void Clear()
        {
            Pages.clear();
            Keys.clear();
            Values.clear();
        }

        NODISCARD FORCEINLINE uint32 Num() const { return (uint32)Keys.size(); }
        NODISCARD FORCEINLINE bool IsEmpty() const { return Keys.empty(); }

        NODISCARD FORCEINLINE FEntity GetKeyAt(uint32 Dense) const { return Keys[Dense]; }
        NODISCARD FORCEINLINE T& GetValueAt(uint32 Dense) { return Values[Dense]; }

    private:

        static constexpr uint32 InvalidDense = Constants::kIndexNoneU32;

        // A real entity never carries the null handle's version, so an empty slot fails the version test.
        NODISCARD FORCEINLINE uint32 FindDense(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            const uint32 Page = Index / PageSize;
            if (Entity.IsNull() || Page >= (uint32)Pages.size() || Pages[Page].empty())
            {
                return InvalidDense;
            }
            const FEntity Slot = Pages[Page][Index % PageSize];
            return Slot.GetVersion() == Entity.GetVersion() ? Slot.GetIndex() : InvalidDense;
        }

        FEntity& AssureSlot(uint32 Index)
        {
            const uint32 Page = Index / PageSize;
            if (Page >= (uint32)Pages.size())
            {
                Pages.resize(Page + 1u);
            }
            if (Pages[Page].empty())
            {
                Pages[Page].resize(PageSize, NullEntity);
            }
            return Pages[Page][Index % PageSize];
        }

        TVector<TVector<FEntity>> Pages;
        TVector<FEntity> Keys;
        TVector<T> Values;
    };
}
