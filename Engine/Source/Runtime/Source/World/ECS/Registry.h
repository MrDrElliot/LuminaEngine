#pragma once

#include "ComponentStorage.h"
#include "Context.h"
#include "Signal.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "View.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

namespace Lumina::ECS
{
    // One world's entities and component pools. Storages are keyed by dense type id, so lookup is an index.
    class FRegistry
    {
    public:

        RUNTIME_API FRegistry();
        RUNTIME_API ~FRegistry();

        FRegistry(const FRegistry&) = delete;
        FRegistry& operator = (const FRegistry&) = delete;

        RUNTIME_API FRegistry(FRegistry&& Other) noexcept;
        RUNTIME_API FRegistry& operator = (FRegistry&& Other) noexcept;


        //~ Entities

        NODISCARD RUNTIME_API FEntity Create();

        // Materializes this exact handle when its slot is free, which is what loading a saved world needs.
        NODISCARD RUNTIME_API FEntity Create(FEntity Hint);

        RUNTIME_API void Destroy(FEntity Entity);

        NODISCARD FORCEINLINE bool IsValid(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            return Index < EntityRecords.size() && EntityRecords[Index] == Entity;
        }

        NODISCARD FORCEINLINE size_t NumEntities() const { return LiveEntityCount; }

        // Destroys every entity and empties every pool. The pools themselves survive.
        RUNTIME_API void Clear();

        RUNTIME_API void Reserve(size_t EntityCount);

        // Trades whole worlds, which is what loading into a pending registry and going live needs.
        RUNTIME_API void Swap(FRegistry& Other) noexcept;

        // Drops tombstones from every stable pool, which is worth doing after a bulk delete.
        RUNTIME_API void Compact();

        // Every live entity carrying none of the listed components, which is how a save skips editor-only ones.
        template<CComponent... TExcluded, typename TFunc>
        void ForEachEntityExcept(TFunc&& Func)
        {
            ForEachEntity([&](FEntity Entity)
            {
                if (!HasAny<TExcluded...>(Entity))
                {
                    Func(Entity);
                }
            });
        }

        template<typename TFunc>
        void ForEachEntity(TFunc&& Func) const
        {
            for (uint32 Index = 0; Index < static_cast<uint32>(EntityRecords.size()); ++Index)
            {
                const FEntity Record = EntityRecords[Index];
                if (Record.GetIndex() == Index)
                {
                    Func(Record);
                }
            }
        }


        //~ Components

        // Hands back a reference for a data component and nothing for a tag, which has no value to hand back.
        template<CComponent T, typename... TArgs>
        typename TComponentStorage<T>::FEmplaceResult Emplace(FEntity Entity, TArgs&&... Args)
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            const TComponentStorage<T> Storage = AssureStorage<T>();
            if constexpr (CDataComponent<T>)
            {
                T& Value = Storage.Emplace(Entity, std::forward<TArgs>(Args)...);
                Storage.GetSet()->Signals.OnConstruct.Broadcast(*this, Entity);
                return Value;
            }
            else
            {
                Storage.Emplace(Entity);
                Storage.GetSet()->Signals.OnConstruct.Broadcast(*this, Entity);
            }
        }

        template<CComponent T, typename... TArgs>
        typename TComponentStorage<T>::FEmplaceResult EmplaceOrReplace(FEntity Entity, TArgs&&... Args)
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            const TComponentStorage<T> Storage = AssureStorage<T>();
            const bool bExisted = Storage.Contains(Entity);
            FComponentSignals& Signals = Storage.GetSet()->Signals;

            if constexpr (CDataComponent<T>)
            {
                T& Value = Storage.EmplaceOrReplace(Entity, std::forward<TArgs>(Args)...);
                (bExisted ? Signals.OnUpdate : Signals.OnConstruct).Broadcast(*this, Entity);
                return Value;
            }
            else
            {
                Storage.EmplaceOrReplace(Entity);
                (bExisted ? Signals.OnUpdate : Signals.OnConstruct).Broadcast(*this, Entity);
            }
        }

        template<CComponent T, typename... TArgs>
        typename TComponentStorage<T>::FEmplaceResult GetOrEmplace(FEntity Entity, TArgs&&... Args)
            requires (CDataComponent<T> || sizeof...(TArgs) == 0)
        {
            const TComponentStorage<T> Storage = AssureStorage<T>();

            if constexpr (CDataComponent<T>)
            {
                if (T* Existing = Storage.TryGet(Entity))
                {
                    return *Existing;
                }
                T& Value = Storage.Emplace(Entity, std::forward<TArgs>(Args)...);
                Storage.GetSet()->Signals.OnConstruct.Broadcast(*this, Entity);
                return Value;
            }
            else
            {
                if (Storage.Contains(Entity))
                {
                    return;
                }
                Storage.Emplace(Entity);
                Storage.GetSet()->Signals.OnConstruct.Broadcast(*this, Entity);
            }
        }

        // A tag has no value to read, so asking for one is a compile error rather than a shared dummy.
        template<CDataComponent T>
        NODISCARD FORCEINLINE T& Get(FEntity Entity)
        {
            return GetStorage<T>().Get(Entity);
        }

        template<CDataComponent T>
        NODISCARD FORCEINLINE const T& Get(FEntity Entity) const
        {
            return const_cast<FRegistry*>(this)->GetStorage<T>().Get(Entity);
        }

        template<CDataComponent T>
        NODISCARD FORCEINLINE T* TryGet(FEntity Entity)
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            return Storage ? Storage.TryGet(Entity) : nullptr;
        }

        template<CDataComponent T>
        NODISCARD FORCEINLINE const T* TryGet(FEntity Entity) const
        {
            return const_cast<FRegistry*>(this)->TryGet<T>(Entity);
        }

        template<CComponent T>
        bool Remove(FEntity Entity)
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            if (!Storage || !Storage.Contains(Entity))
            {
                return false;
            }
            Storage.GetSet()->Signals.OnDestroy.Broadcast(*this, Entity);
            return Storage.RemoveEntity(Entity);
        }

        template<CComponent... Ts>
        NODISCARD FORCEINLINE bool HasAll(FEntity Entity) const
        {
            static_assert(AreAllDistinct<Ts...>(), "HasAll lists each component once.");
            return (HasOne<Ts>(Entity) && ...);
        }

        template<CComponent... Ts>
        NODISCARD FORCEINLINE bool HasAny(FEntity Entity) const
        {
            static_assert(AreAllDistinct<Ts...>(), "HasAny lists each component once.");
            return (HasOne<Ts>(Entity) || ...);
        }

        // Fires OnUpdate after the caller has written through the returned reference.
        template<CDataComponent T>
        T& Patch(FEntity Entity, auto&&... Mutators)
        {
            const TComponentStorage<T> Storage = GetStorage<T>();
            T& Value = Storage.Get(Entity);
            (std::forward<decltype(Mutators)>(Mutators)(Value), ...);
            Storage.GetSet()->Signals.OnUpdate.Broadcast(*this, Entity);
            return Value;
        }

        template<CComponent T>
        void ClearComponent()
        {
            if (const TComponentStorage<T> Storage = FindStorage<T>())
            {
                Storage.GetSet()->ClearAll();
            }
        }

        // Pre-sizes one pool, which removes the relocation a growing packed pool would otherwise pay.
        template<CComponent T>
        void ReserveComponents(size_t Count)
        {
            AssureStorage<T>().Reserve(Count);
        }


        //~ Storages

        template<CComponent T>
        NODISCARD TComponentStorage<T> AssureStorage()
        {
            const FComponentTypeID TypeID = GetComponentTypeID<T>();
            if (TypeID >= StoragesByTypeID.size() || StoragesByTypeID[TypeID] == nullptr)
            {
                CreateStorage(TypeID);
            }
            return TComponentStorage<T>(StoragesByTypeID[TypeID]);
        }

        template<CComponent T>
        NODISCARD FORCEINLINE TComponentStorage<T> FindStorage() const
        {
            return TComponentStorage<T>(FindStorage(GetComponentTypeID<T>()));
        }

        template<CComponent T>
        NODISCARD FORCEINLINE TComponentStorage<T> GetStorage()
        {
            return AssureStorage<T>();
        }

        NODISCARD FORCEINLINE FSparseSet* FindStorage(uint32 TypeID) const
        {
            return TypeID < StoragesByTypeID.size() ? StoragesByTypeID[TypeID] : nullptr;
        }

        // Only the pools this registry actually created, so a type-erased pass never walks the empty ones.
        NODISCARD FORCEINLINE const TVector<FSparseSet*>& GetActiveStorages() const { return ActiveStorages; }

        // A second pool of the same type under its own name, which is how one tag becomes many.
        template<CComponent T>
        NODISCARD TComponentStorage<T> NamedStorage(const FName& Name)
        {
            return TComponentStorage<T>(AssureNamedStorage(GetComponentTypeID<T>(), Name));
        }

        template<CComponent T>
        NODISCARD TComponentStorage<T> FindNamedStorage(const FName& Name) const
        {
            return TComponentStorage<T>(FindNamedStorage(GetComponentTypeID<T>(), Name));
        }

        NODISCARD RUNTIME_API FSparseSet* FindNamedStorage(FComponentTypeID TypeID, const FName& Name) const;


        //~ Context

        NODISCARD FORCEINLINE FRegistryContext& Ctx() { return Context; }
        NODISCARD FORCEINLINE const FRegistryContext& Ctx() const { return Context; }

        template<typename T, typename... TArgs>
        T& EmplaceSingleton(TArgs&&... Args) { return Context.Emplace<T>(std::forward<TArgs>(Args)...); }

        template<typename T> NODISCARD FORCEINLINE T& GetSingleton() { return Context.Get<T>(); }
        template<typename T> NODISCARD FORCEINLINE const T& GetSingleton() const { return Context.Get<T>(); }
        template<typename T> NODISCARD FORCEINLINE T* TryGetSingleton() { return Context.Find<T>(); }
        template<typename T> NODISCARD FORCEINLINE const T* TryGetSingleton() const { return Context.Find<T>(); }
        template<typename T> NODISCARD FORCEINLINE bool HasSingleton() const { return Context.Contains<T>(); }
        template<typename T> bool EraseSingleton() { return Context.Erase<T>(); }


        //~ Signals

        template<CComponent T>
        NODISCARD FComponentSignals& GetSignals() { return AssureStorage<T>().GetSet()->Signals; }

        NODISCARD FComponentSignal& OnEntityCreated()   { return EntityCreated; }
        NODISCARD FComponentSignal& OnEntityDestroyed() { return EntityDestroyed; }


        //~ Views

        template<CComponent... TInclude>
        NODISCARD TView<TExclude<>, TInclude...> View()
        {
            return TView<TExclude<>, TInclude...>(AssureStorage<TInclude>()...);
        }

        template<CComponent... TInclude, CComponent... TExcl>
        NODISCARD TView<TExclude<TExcl...>, TInclude...> View(TExclude<TExcl...>)
        {
            TView<TExclude<TExcl...>, TInclude...> Result(AssureStorage<TInclude>()...);
            Result.BindExcludes(AssureStorage<TExcl>().GetSet()...);
            return Result;
        }

    private:

        template<CComponent T>
        NODISCARD FORCEINLINE bool HasOne(FEntity Entity) const
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            return Storage && Storage.Contains(Entity);
        }

        RUNTIME_API void CreateStorage(FComponentTypeID TypeID);
        RUNTIME_API FSparseSet* AssureNamedStorage(FComponentTypeID TypeID, const FName& Name);

        NODISCARD static uint64 MakeNamedStorageKey(FComponentTypeID TypeID, const FName& Name);
        RUNTIME_API void DetachFromAllStorages(FEntity Entity);

        // Slot index maps to a record whose version is current. A dead slot links the next free index.
        TVector<FEntity> EntityRecords;

        static constexpr uint32 NoFreeSlot = FEntity::IndexMask;
        uint32 FreeEntityHead = NoFreeSlot;
        size_t LiveEntityCount = 0;

        TVector<TUniquePtr<FSparseSet>> OwnedStorages;
        TVector<FSparseSet*> StoragesByTypeID;
        TVector<FSparseSet*> ActiveStorages;

        // Cold, so a hash map is the right shape here even though the primary pools are an array.
        THashMap<uint64, TUniquePtr<FSparseSet>> NamedStorages;

        FRegistryContext Context;

        FComponentSignal EntityCreated;
        FComponentSignal EntityDestroyed;
    };
}
