#pragma once

#include "ComponentStorage.h"
#include "Context.h"
#include "Hierarchy.h"
#include "Signal.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "View.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

#include "Containers/Algorithm.h"

namespace Lumina::ECS
{
    struct FEntityAllocState;

    // Moving it hands the source a fresh state, so a moved-from registry can still create entities.
    struct RUNTIME_API FEntityAllocHolder
    {
        FEntityAllocHolder();
        ~FEntityAllocHolder();
        FEntityAllocHolder(FEntityAllocHolder&& Other) noexcept;
        FEntityAllocHolder& operator = (FEntityAllocHolder&& Other) noexcept;
        FEntityAllocHolder(const FEntityAllocHolder&) = delete;
        FEntityAllocHolder& operator = (const FEntityAllocHolder&) = delete;

        FORCEINLINE FEntityAllocState* operator->() const { return State.Get(); }
        void Swap(FEntityAllocHolder& Other) noexcept;

        TUniquePtr<FEntityAllocState> State;
    };

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

        // Never recycles a freed slot, so an editor undo that restores a deleted handle still finds it free.
        NODISCARD RUNTIME_API FEntity CreateInFreshSlot();

        RUNTIME_API void Destroy(FEntity Entity);

        // Thread-safe. The handle is not valid until MaterializeReserved, so a deferred create can be targeted before it exists.
        NODISCARD RUNTIME_API FEntity ReserveEntity();

        // Makes a reserved handle live and fires the created signal, exactly as Create would have.
        RUNTIME_API void MaterializeReserved(FEntity Entity);

        // Returns a reservation that will never be materialized to the free list.
        RUNTIME_API void ReleaseReserved(FEntity Entity);

        NODISCARD FORCEINLINE bool IsValid(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            return Index < static_cast<uint32>(EntityRecords.Num()) && EntityRecords[Index] == Entity;
        }

        NODISCARD FORCEINLINE int32 NumEntities() const { return LiveEntityCount; }

        // Rises with every create, so it orders live entities by creation even where a later one reused a lower slot.
        NODISCARD FORCEINLINE uint64 GetCreationSerial(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            return Index < static_cast<uint32>(CreationSerials.Num()) ? CreationSerials[Index] : 0u;
        }

        // Destroys every entity and empties every pool. The pools themselves survive.
        RUNTIME_API void Clear();

        RUNTIME_API void Reserve(int32 EntityCount);

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
            for (int32 Index = 0; Index < EntityRecords.Num(); ++Index)
            {
                const FEntity Record = EntityRecords[Index];
                if (Record.GetIndex() == static_cast<uint32>(Index))
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
                T& Value = Storage.Emplace(Entity, Forward<TArgs>(Args)...);
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
                T& Value = Storage.EmplaceOrReplace(Entity, Forward<TArgs>(Args)...);
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
                T& Value = Storage.Emplace(Entity, Forward<TArgs>(Args)...);
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

        // T and Write<T> are mutable and stamp a tracked pool; Read<T>, or any Get on a const registry, is const. A tag has no value, so Get on one does not compile.
        template<typename T> requires CDataComponent<TBareComponent<T>>
        NODISCARD FORCEINLINE auto& Get(FEntity Entity)
        {
            if constexpr (std::is_const_v<TViewType<T>>)
            {
                return std::as_const(*this).template Get<TBareComponent<T>>(Entity);
            }
            else
            {
                // FindStorage rather than AssureStorage, so a read never creates a pool and never needs a call.
                const TComponentStorage<TBareComponent<T>> Storage = FindStorage<TBareComponent<T>>();
                DEBUG_ASSERT(Storage.IsValid(), "Get on a component type this registry has no pool for");
                return Storage.Get(Entity);
            }
        }

        // A const registry cannot hand out ECS::Write access.
        template<typename T> requires (CDataComponent<TBareComponent<T>> && !bIsWriteAccess<T>)
        NODISCARD FORCEINLINE const TBareComponent<T>& Get(FEntity Entity) const
        {
            const TComponentStorage<TBareComponent<T>> Storage = FindStorage<TBareComponent<T>>();
            DEBUG_ASSERT(Storage.IsValid(), "Get on a component type this registry has no pool for");
            return Storage.GetUntracked(Entity);
        }

        template<typename T> requires CDataComponent<TBareComponent<T>>
        NODISCARD FORCEINLINE auto* TryGet(FEntity Entity)
        {
            if constexpr (std::is_const_v<TViewType<T>>)
            {
                return std::as_const(*this).template TryGet<TBareComponent<T>>(Entity);
            }
            else
            {
                const TComponentStorage<TBareComponent<T>> Storage = FindStorage<TBareComponent<T>>();
                return Storage ? Storage.TryGet(Entity) : nullptr;
            }
        }

        template<typename T> requires (CDataComponent<TBareComponent<T>> && !bIsWriteAccess<T>)
        NODISCARD FORCEINLINE const TBareComponent<T>* TryGet(FEntity Entity) const
        {
            const TComponentStorage<TBareComponent<T>> Storage = FindStorage<TBareComponent<T>>();
            return Storage ? Storage.TryGetUntracked(Entity) : nullptr;
        }

        // Several at once, for structured bindings: auto [Position, Velocity] = Registry.Get<ECS::Write<SPos>, ECS::Read<SVel>>(Entity).
        template<typename T0, typename T1, typename... TRest>
        NODISCARD FORCEINLINE auto Get(FEntity Entity)
        {
            return TTuple<decltype(Get<T0>(Entity)), decltype(Get<T1>(Entity)), decltype(Get<TRest>(Entity))...>(Get<T0>(Entity), Get<T1>(Entity), Get<TRest>(Entity)...);
        }

        template<typename T0, typename T1, typename... TRest>
        NODISCARD FORCEINLINE auto Get(FEntity Entity) const
        {
            return TTuple<decltype(Get<T0>(Entity)), decltype(Get<T1>(Entity)), decltype(Get<TRest>(Entity))...>(Get<T0>(Entity), Get<T1>(Entity), Get<TRest>(Entity)...);
        }

        // Null where the entity lacks that component.
        template<typename T0, typename T1, typename... TRest>
        NODISCARD FORCEINLINE auto TryGet(FEntity Entity)
        {
            return TTuple<decltype(TryGet<T0>(Entity)), decltype(TryGet<T1>(Entity)), decltype(TryGet<TRest>(Entity))...>(TryGet<T0>(Entity), TryGet<T1>(Entity), TryGet<TRest>(Entity)...);
        }

        template<typename T0, typename T1, typename... TRest>
        NODISCARD FORCEINLINE auto TryGet(FEntity Entity) const
        {
            return TTuple<decltype(TryGet<T0>(Entity)), decltype(TryGet<T1>(Entity)), decltype(TryGet<TRest>(Entity))...>(TryGet<T0>(Entity), TryGet<T1>(Entity), TryGet<TRest>(Entity)...);
        }

        // Removes each listed component the entity holds, in the order listed, and returns whether any was removed.
        template<CComponent... Ts> requires (sizeof...(Ts) > 0)
        bool Remove(FEntity Entity)
        {
            static_assert(AreAllDistinct<Ts...>(), "Remove lists each component once.");
            bool bRemoved = false;
            ((bRemoved |= RemoveOne<Ts>(Entity)), ...);
            return bRemoved;
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
            DEBUG_ASSERT(Storage.Contains(Entity), "Patch on a component the entity does not have");
            const uint32 DenseIndex = Storage.GetSet()->GetDenseIndex(Entity);
            T& Value = Storage.GetAtDense(DenseIndex);
            (Forward<decltype(Mutators)>(Mutators)(Value), ...);
            Storage.GetSet()->MarkWritten(DenseIndex);
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

        //~ The same operations keyed by the dense type id, for a caller that knows the type only at runtime.

        RUNTIME_API FSparseSet& AssureStorage(FComponentTypeID TypeID);

        // The existing value, or a default one emplaced with OnConstruct fired. Null for a tag.
        RUNTIME_API void* GetOrEmplaceRaw(FComponentTypeID TypeID, FEntity Entity);

        // A copy of Source, or a default value when Source is null, firing OnConstruct when added and OnUpdate when replaced.
        RUNTIME_API void* EmplaceOrReplaceRaw(FComponentTypeID TypeID, FEntity Entity, const void* Source);

        RUNTIME_API bool RemoveRaw(FComponentTypeID TypeID, FEntity Entity);

        // Fires OnUpdate for an entity that holds the component, after the caller wrote through its pointer.
        RUNTIME_API void PatchRaw(FComponentTypeID TypeID, FEntity Entity);

        // Records a write through a raw pointer like PatchRaw, but fires no signal, for a store that must stay silent such as a live edit.
        RUNTIME_API void MarkChangedRaw(FComponentTypeID TypeID, FEntity Entity);

        // Pre-sizes one pool, which removes the relocation a growing packed pool would otherwise pay.
        template<CComponent T>
        void ReserveComponents(int32 Count)
        {
            AssureStorage<T>().Reserve(Count);
        }

        // Room for one more T per entity holding TFor, so adding a T to each inside a view of TFor never grows the pool.
        template<CComponent T, CComponent TFor>
        void ReserveComponentsFor()
        {
            const TComponentStorage<T> Storage = AssureStorage<T>();
            Storage.Reserve(Storage.GetDenseSize() + AssureStorage<TFor>().GetDenseSize());
        }


        //~ Storages

        template<CComponent T>
        NODISCARD FORCEINLINE TComponentStorage<T> AssureStorage()
        {
            const FComponentTypeID TypeID = GetComponentTypeID<T>();
            if (TypeID >= StoragesByTypeID.Num() || StoragesByTypeID[TypeID] == nullptr)
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
            return TypeID < static_cast<uint32>(StoragesByTypeID.Num()) ? StoragesByTypeID[TypeID] : nullptr;
        }

        // Only the pools this registry actually created, so a type-erased pass never walks the empty ones.
        NODISCARD FORCEINLINE const TVector<FSparseSet*>& GetActiveStorages() const { return ActiveStorages; }

        // Creates every listed pool now, so code about to go wide can build views on workers without creating one there.
        template<CViewComponent... Ts>
        void AssureStorages()
        {
            ((void)AssureStorage<TBareComponent<Ts>>(), ...);
        }

        RUNTIME_API void AssureStorages(TSpan<const FComponentTypeID> TypeIDs);

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


        //~ Groups

        // Owns every listed pool and keeps the entities holding all of them packed at the same front slots of each, so a TView or TQuery over exactly these includes walks aligned arrays with no membership probes. Null, with an error logged, when a listed type is already owned by a different group or its pool is in-place-delete; declaring the same set again hands back the existing group.
        template<CComponent... Ts>
        FOwningGroup* DeclareGroup()
        {
            static_assert(sizeof...(Ts) >= 2, "A group owns at least two component types; a single pool is already packed.");
            static_assert(AreAllDistinct<Ts...>(), "A group lists each component once.");
            const FComponentTypeID TypeIDs[] = { GetComponentTypeID<Ts>()... };
            return DeclareGroup(TSpan<const FComponentTypeID>(TypeIDs, sizeof...(Ts)));
        }

        RUNTIME_API FOwningGroup* DeclareGroup(TSpan<const FComponentTypeID> TypeIDs);

        // Null when T has no pool or no group owns it.
        template<CComponent T>
        NODISCARD const FOwningGroup* FindOwningGroup() const
        {
            const FSparseSet* Set = FindStorage(GetComponentTypeID<T>());
            return Set != nullptr ? Set->GetOwningGroup() : nullptr;
        }

        // Orders T's pool by Compare(const T&, const T&); an owned pool sorts its group front through every owned pool and its tail alone, so the group stays packed and aligned.
        template<CDataComponent T, typename TCompare>
        void Sort(TCompare Compare)
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            if (!Storage)
            {
                return;
            }
            FSparseSet* Set = Storage.GetSet();
            Set->Compact();
            if (Set->HasTombstones())
            {
                return;
            }

            const FOwningGroup* Group = Set->GetOwningGroup();
            const int32 Split = Group != nullptr ? Group->Num : 0;
            TVector<int32> Order;
            auto SortRange = [&](int32 Begin, int32 End, TSpan<FSparseSet* const> Sets)
            {
                Order.SetNumUninitialized(End - Begin);
                for (int32 Index = 0; Index < Order.Num(); ++Index)
                {
                    Order[Index] = Begin + Index;
                }
                Algo::Sort(Order, [&](int32 A, int32 B)
                {
                    return Compare(std::as_const(Storage.GetAtDense(static_cast<uint32>(A))), std::as_const(Storage.GetAtDense(static_cast<uint32>(B))));
                });
                PermuteDense(Sets, Begin, Order);
            };

            if (Group != nullptr)
            {
                SortRange(0, Split, TSpan<FSparseSet* const>(Group->Owned.GetData(), Group->Owned.Num()));
            }
            FSparseSet* const Self[] = { Set };
            SortRange(Split, Set->GetDenseSize(), TSpan<FSparseSet* const>(Self, 1));
        }


        //~ Hierarchy

        NODISCARD FORCEINLINE const FHierarchy& GetHierarchy() const { return Hierarchy; }

        // Puts Child at Position among Parent's children, detaching it from any old parent. Refuses a cycle or a dead handle.
        RUNTIME_API bool AttachChild(FEntity Child, FEntity Parent, uint32 Position = FHierarchy::AppendPosition);

        RUNTIME_API void DetachFromParent(FEntity Child);

        // Off by default, which orphans the children of a destroyed entity instead of destroying them.
        void SetDestroyDescendantsWithParent(bool bEnable) { bDestroyDescendantsWithParent = bEnable; }

        // Fires for an entity whose parent changed and for each surviving parent that gained or lost a child.
        NODISCARD FComponentSignal& OnHierarchyChanged() { return HierarchyChanged; }


        //~ Change detection

        // Zero for a type with no pool, and never creates one. What moves it is listed on FSparseSet::GetWriteEpoch.
        template<CComponent T>
        NODISCARD FORCEINLINE uint64 GetWriteEpoch() const
        {
            return GetWriteEpoch(GetComponentTypeID<T>());
        }

        NODISCARD FORCEINLINE uint64 GetWriteEpoch(FComponentTypeID TypeID) const
        {
            const FSparseSet* Storage = FindStorage(TypeID);
            return Storage != nullptr ? Storage->GetWriteEpoch() : 0u;
        }

        // Keeps a change tick per element of T from now on, creating the pool if needed. Main thread only, like any pool creation.
        template<CComponent T>
        void TrackChanges()
        {
            AssureStorage<T>().GetSet()->TrackChanges();
        }

        // Opens a new change tick and expires references cached into tracked pools, so their next write stamps. Main thread, between ticks.
        RUNTIME_API uint32 AdvanceChangeTick();


        //~ Context

        template<typename Self> NODISCARD FORCEINLINE auto& Ctx(this Self&& S) { return S.Context; }

        template<typename T, typename... TArgs>
        T& EmplaceSingleton(TArgs&&... Args) { return Context.Emplace<T>(Forward<TArgs>(Args)...); }

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

        // View<ECS::Read<A>, ECS::Write<B>, ECS::With<C>, ECS::Without<D>>(). Creates any missing pool, so call it on the main thread.
        template<CViewTerm... TTerms>
        NODISCARD typename TViewOfTerms<TTerms...>::Type View()
        {
            return TViewOfTerms<TTerms...>::Assure(*this);
        }

        template<CViewComponent... TInclude, CComponent... TExcl>
        NODISCARD TView<TExclude<TExcl...>, TViewInclude<TInclude>...> View(TExclude<TExcl...>)
        {
            TView<TExclude<TExcl...>, TViewInclude<TInclude>...> Result(AssureStorage<TBareComponent<TInclude>>()...);
            Result.BindFilters(AssureStorage<TExcl>().GetSet()...);
            return Result;
        }

        // Never creates a pool: a missing include leaves the view empty and a missing exclude excludes nothing, so any thread may build one.
        template<CViewTerm... TTerms>
        NODISCARD typename TViewOfTerms<TTerms...>::Type FindView() const
        {
            return TViewOfTerms<TTerms...>::Find(*this);
        }

        template<CViewComponent... TInclude, CComponent... TExcl>
        NODISCARD TView<TExclude<TExcl...>, TViewInclude<TInclude>...> FindView(TExclude<TExcl...>) const
        {
            TView<TExclude<TExcl...>, TViewInclude<TInclude>...> Result(FindStorage<TBareComponent<TInclude>>()...);
            Result.BindFilters(FindStorage<TExcl>().GetSet()...);
            return Result;
        }

    private:

        // Makes every reference cached into a tracked pool resolve again, which is what lets a write through one stamp.
        RUNTIME_API void ExpireEscapedPointers();

        template<CComponent T>
        NODISCARD FORCEINLINE bool HasOne(FEntity Entity) const
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            return Storage && Storage.Contains(Entity);
        }

        template<CComponent T>
        bool RemoveOne(FEntity Entity)
        {
            const TComponentStorage<T> Storage = FindStorage<T>();
            if (!Storage || !Storage.Contains(Entity))
            {
                return false;
            }
            Storage.GetSet()->Signals.OnDestroy.Broadcast(*this, Entity);
            return Storage.RemoveEntity(Entity);
        }

        RUNTIME_API void CreateStorage(FComponentTypeID TypeID);
        RUNTIME_API void PermuteDense(TSpan<FSparseSet* const> Sets, int32 Begin, TSpan<int32> Order);
        void CheckPoolTableThread();
        RUNTIME_API FSparseSet* AssureNamedStorage(FComponentTypeID TypeID, const FName& Name);

        NODISCARD static uint64 MakeNamedStorageKey(FComponentTypeID TypeID, const FName& Name);
        RUNTIME_API void DetachFromAllStorages(FEntity Entity);
        void DestroyHierarchyLinks(FEntity Entity);

        // Caller holds the allocation lock.
        uint32 TakeSlotLocked(uint32& OutVersion);
        void EnsureRecordLocked(uint32 Index);

        void NoteCreated(FEntity Entity);

        // A dead slot links the next free index, and a reserved one holds NoFreeSlot so it is neither live nor free.
        TVector<FEntity> EntityRecords;

        TVector<uint64> CreationSerials;
        uint64 LastCreationSerial = 0;

        // Behind a pointer so the registry stays movable.
        FEntityAllocHolder AllocState;

        static constexpr uint32 NoFreeSlot = FEntity::IndexMask;
        uint32 FreeEntityHead = NoFreeSlot;
        int32 LiveEntityCount = 0;

        // Mid-destroy, so a handler destroying the same entity again is ignored rather than freeing its record twice.
        TVector<FEntity> DestroyingEntities;

        // Before the pools, so every pool that points at a group dies first.
        TVector<TUniquePtr<FOwningGroup>> Groups;

        TVector<TUniquePtr<FSparseSet>> OwnedStorages;
        TVector<FSparseSet*> StoragesByTypeID;
        TVector<FSparseSet*> ActiveStorages;

        // The thread that created the first pool, which with the main thread is all that may grow the pool table. Zero until claimed.
        uint64 PoolTableOwner = 0;

        // Cold, so a hash map is the right shape here even though the primary pools are an array.
        THashMap<uint64, TUniquePtr<FSparseSet>> NamedStorages;

        FRegistryContext Context;

        FHierarchy Hierarchy;
        bool       bDestroyDescendantsWithParent = false;
        FComponentSignal HierarchyChanged;

        FComponentSignal EntityCreated;
        FComponentSignal EntityDestroyed;
    };
}
