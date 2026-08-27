#pragma once

#include "ComponentStorage.h"
#include "Containers/Tuple.h"
#include "Containers/Vector.h"

#include <utility>

namespace Lumina::ECS
{
    // Filter tag, so a view spells its exclusions as View<A, B>(TExclude<C>{}).
    template<typename... Ts>
    struct TExclude {};

    // Typed multi-component iteration. The pack drives which storages it holds, nothing else.
    template<typename TExcludeList, typename... TInclude>
    class TView;

    template<typename... TExcl, typename... TInclude>
    class TView<TExclude<TExcl...>, TInclude...>
    {
        static_assert(sizeof...(TInclude) > 0, "A view needs at least one included component.");
        static_assert((CComponent<TInclude> && ...), "A view includes component types, not references or pointers.");
        static_assert((CComponent<TExcl> && ...), "A view excludes component types, not references or pointers.");
        static_assert(AreAllDistinct<TInclude...>(), "A view lists each included component once.");
        static_assert(AreAllDistinct<TExcl...>(), "A view lists each excluded component once.");
        static_assert(!(bIsOneOf<TInclude, TExcl...> || ...),
            "A view cannot both include and exclude the same component; it would never match anything.");

    public:

        static constexpr size_t IncludeCount = sizeof...(TInclude);
        static constexpr size_t ExcludeCount = sizeof...(TExcl);

        template<size_t Slot>
        using TIncludeAt = TTupleElementT<Slot, TTuple<TInclude...>>;

        // Tags carry membership only, so the callback sees the data components and nothing else.
        static constexpr size_t DataCount = ((CDataComponent<TInclude> ? 1u : 0u) + ... + 0u);

        // One included pool means the dense walk already addresses every element, filter or not.
        static constexpr bool bIsDirectScan = (IncludeCount == 1);

        TView() = default;

        explicit TView(TComponentStorage<TInclude>... InIncludes)
            : Includes(InIncludes...)
        {
            PickDriver();
        }

        // Separate from the constructor because one signature cannot carry two parameter packs.
        template<typename... TSets>
        void BindExcludes(TSets*... Sets)
        {
            size_t Slot = 0;
            ((Excludes[Slot++] = static_cast<const FSparseSet*>(Sets)), ...);
        }

        NODISCARD FORCEINLINE bool IsEmpty() const { return Driver == nullptr || Driver->Num() == 0; }

        // Upper bound only. The driver is the smallest included pool, so the real count is at most this.
        NODISCARD FORCEINLINE size_t NumCandidates() const { return Driver != nullptr ? Driver->Num() : 0; }

        // Dense slots, not live entities, because splitting on the live count drops a tombstoned tail.
        NODISCARD FORCEINLINE size_t NumDenseSlots() const { return Driver != nullptr ? Driver->GetDenseSize() : 0; }

        NODISCARD FORCEINLINE const FSparseSet* GetDriver() const { return Driver; }

        // The driver bounds the result, so this is an upper bound rather than a count.
        NODISCARD FORCEINLINE size_t Num() const { return NumCandidates(); }

        // Range-for support, so a call site can walk entities without a callback.
        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const TView* InView, size_t InIndex)
                : View(InView)
                , Index(InIndex)
            {
                Advance();
            }

            NODISCARD FEntity operator * () const { return View->GetDriver()->GetDenseData()[Index]; }

            FIterator& operator ++ () { ++Index; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return Index == Other.Index; }
            NODISCARD bool operator != (const FIterator& Other) const { return Index != Other.Index; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FSparseSet* Set = View->GetDriver();
                const size_t Count = Set->GetDenseSize();
                const FEntity* Dense = Set->GetDenseData();

                while (Index < Count && (Dense[Index].IsTombstone() || !View->Contains(Dense[Index])))
                {
                    ++Index;
                }
            }

            const TView* View = nullptr;
            size_t Index = 0;
        };

        NODISCARD FIterator begin() const { return FIterator(this, 0); }
        NODISCARD FIterator end() const { return FIterator(this, GetDriver() != nullptr ? GetDriver()->GetDenseSize() : 0); }

        // Tuple iteration, so a range-for can bind the entity and its components and still use break.
        class FEachIterator
        {
        public:

            FEachIterator() = default;

            FEachIterator(const TView* InView, size_t InIndex)
                : View(InView)
                , Index(InIndex)
            {
                Advance();
            }

            NODISCARD auto operator * () const
            {
                const FEntity Entity = View->GetDriver()->GetDenseData()[Index];
                return TupleCat(TTuple<FEntity>(Entity),
                    View->RefTuple(Entity, std::index_sequence_for<TInclude...>{}));
            }

            FEachIterator& operator ++ () { ++Index; Advance(); return *this; }

            NODISCARD bool operator == (const FEachIterator& Other) const { return Index == Other.Index; }
            NODISCARD bool operator != (const FEachIterator& Other) const { return Index != Other.Index; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FSparseSet* Set = View->GetDriver();
                const size_t Count = Set->GetDenseSize();
                const FEntity* Dense = Set->GetDenseData();

                while (Index < Count && (Dense[Index].IsTombstone() || !View->Contains(Dense[Index])))
                {
                    ++Index;
                }
            }

            const TView* View = nullptr;
            size_t Index = 0;
        };

        struct FEachRange
        {
            const TView* View = nullptr;

            NODISCARD FEachIterator begin() const { return FEachIterator(View, 0); }

            NODISCARD FEachIterator end() const
            {
                const FSparseSet* Set = View->GetDriver();
                return FEachIterator(View, Set != nullptr ? Set->GetDenseSize() : 0);
            }
        };

        NODISCARD FEachRange Each() const { return FEachRange{ this }; }

        // Every included data component at once, so a caller can Apply over the pack.
        NODISCARD auto GetAll(FEntity Entity) const
        {
            return RefTuple(Entity, std::index_sequence_for<TInclude...>{});
        }

        template<typename T>
            requires (bIsOneOf<T, TInclude...> && CDataComponent<T>)
        NODISCARD FORCEINLINE T& Get(FEntity Entity) const
        {
            return GetStorage<T>().Get(Entity);
        }

        template<typename T>
            requires (bIsOneOf<T, TInclude...> && CDataComponent<T>)
        NODISCARD FORCEINLINE T* TryGet(FEntity Entity) const
        {
            return GetStorage<T>().TryGet(Entity);
        }

        NODISCARD bool Contains(FEntity Entity) const
        {
            if (Driver == nullptr)
            {
                return false;
            }
            return MatchesIncludes(Entity) && !MatchesAnyExclude(Entity);
        }

        // By value all the way down, so the functor's captures stay in registers across the scan.
        template<typename TFunc>
        FORCEINLINE void ForEach(TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            if constexpr (bIsDirectScan)
            {
                ForEachDirect(Func);
            }
            else
            {
                ForEachFiltered(Func);
            }
        }

        // A slice of the driver's dense array for one parallel job, so split on NumDenseSlots().
        template<typename TFunc>
        FORCEINLINE void ForEachInRange(size_t Begin, size_t End, TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            const FEntity* DenseData = Driver->GetDenseData();
            const size_t DenseSize = Driver->GetDenseSize();
            const size_t Last = End < DenseSize ? End : DenseSize;
            const FExcludeProbes Probes = MakeExcludeProbes();

            for (size_t Index = Begin; Index < Last; ++Index)
            {
                const FEntity Entity = DenseData[Index];
                if (Entity.IsTombstone())
                {
                    continue;
                }

                if constexpr (ExcludeCount > 0)
                {
                    if (Probes.Rejects(Entity))
                    {
                        continue;
                    }
                }

                if (!TagsPresent(Entity, std::index_sequence_for<TInclude...>{}))
                {
                    continue;
                }

                // One lookup per data pool, reused as the callback argument, so membership is never tested twice.
                auto Values = Gather(Entity, static_cast<uint32>(Index), std::index_sequence_for<TInclude...>{});

                const bool bComplete = Apply([](auto*... Value) { return ((Value != nullptr) && ... && true); }, Values);
                if (!bComplete)
                {
                    continue;
                }

                Apply([&Func, Entity](auto*... Value) { InvokeEntityCallback(Func, Entity, *Value...); }, Values);
            }
        }

        // Entity-only iteration, for a caller that reaches its components through Get.
        template<typename TFunc>
        FORCEINLINE void ForEachEntity(TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            const FEntity* DenseData = Driver->GetDenseData();
            const size_t DenseSize = Driver->GetDenseSize();

            // A packed single pool has nothing to filter and no holes, so the walk is just the array.
            if constexpr (IncludeCount == 1 && ExcludeCount == 0)
            {
                if (!Driver->HasTombstones())
                {
                    for (size_t Index = 0; Index < DenseSize; ++Index)
                    {
                        Func(DenseData[Index]);
                    }
                    return;
                }
            }

            for (size_t Index = 0; Index < DenseSize; ++Index)
            {
                const FEntity Entity = DenseData[Index];
                if (Entity.IsTombstone())
                {
                    continue;
                }
                if (!MatchesRest(Entity))
                {
                    continue;
                }
                Func(Entity);
            }
        }

    private:

        template<typename T>
        NODISCARD FORCEINLINE const TComponentStorage<T>& GetStorage() const
        {
            return Lumina::Get<TComponentStorage<T>>(Includes);
        }

        void PickDriver()
        {
            Driver = nullptr;
            DriverSlot = 0;

            size_t Slot = 0;
            Apply([this, &Slot](const auto&... Storage)
            {
                ((Storage.IsValid() ? ConsiderDriver(Storage.GetSet(), Slot) : void(), ++Slot), ...);
            }, Includes);

            // A missing pool means the view can never match, so it stays empty rather than driving off another.
            const bool bAnyMissing = Apply([](const auto&... Storage) { return (!Storage.IsValid() || ...); }, Includes);
            if (bAnyMissing)
            {
                Driver = nullptr;
            }
        }

        void ConsiderDriver(const FSparseSet* Candidate, size_t Slot)
        {
            if (Driver == nullptr || Candidate->Num() < Driver->Num())
            {
                Driver = Candidate;
                DriverSlot = Slot;
            }
        }

        NODISCARD FORCEINLINE bool MatchesAnyExclude(FEntity Entity) const
        {
            if constexpr (ExcludeCount == 0)
            {
                return false;
            }
            else
            {
                for (size_t Index = 0; Index < ExcludeCount; ++Index)
                {
                    if (Excludes[Index] != nullptr && Excludes[Index]->Contains(Entity))
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        // The probes are loop-invariant, so a scan pays for the page arrays once rather than per element.
        struct FExcludeProbes
        {
            FMembershipProbe Probes[ExcludeCount > 0 ? ExcludeCount : 1];

            NODISCARD FORCEINLINE bool Rejects(FEntity Entity) const
            {
                for (size_t Index = 0; Index < ExcludeCount; ++Index)
                {
                    if (Probes[Index].Contains(Entity))
                    {
                        return true;
                    }
                }
                return false;
            }
        };

        NODISCARD FExcludeProbes MakeExcludeProbes() const
        {
            FExcludeProbes Result;
            for (size_t Index = 0; Index < ExcludeCount; ++Index)
            {
                if (Excludes[Index] != nullptr)
                {
                    Result.Probes[Index] = Excludes[Index]->MakeProbe();
                }
            }
            return Result;
        }


        // A tag carries no value, so it never reaches the tuple the iterator hands back.
        template<size_t Slot>
        NODISCARD auto RefSlot(FEntity Entity) const
        {
            using TSlot = TIncludeAt<Slot>;
            if constexpr (CDataComponent<TSlot>)
            {
                return TTuple<TSlot&>(Get<TSlot>(Entity));
            }
            else
            {
                return TTuple<>{};
            }
        }

        template<size_t... Slots>
        NODISCARD auto RefTuple(FEntity Entity, std::index_sequence<Slots...>) const
        {
            return TupleCat(RefSlot<Slots>(Entity)...);
        }

        NODISCARD FORCEINLINE bool MatchesIncludes(FEntity Entity) const
        {
            return Apply([Entity](const auto&... Storage)
            {
                return ((Storage.IsValid() && Storage.Contains(Entity)) && ...);
            }, Includes);
        }

        // Everything except the driver's own membership, which the dense walk already established.
        NODISCARD FORCEINLINE bool MatchesRest(FEntity Entity) const
        {
            if constexpr (IncludeCount > 1)
            {
                const bool bAllPresent = Apply([this, Entity](const auto&... Storage)
                {
                    return ((Storage.GetSet() == Driver || Storage.Contains(Entity)) && ...);
                }, Includes);

                if (!bAllPresent)
                {
                    return false;
                }
            }

            return !MatchesAnyExclude(Entity);
        }

        template<typename TFunc>
        FORCEINLINE void ForEachDirect(TFunc Func) const
        {
            if constexpr (ExcludeCount == 0)
            {
                Lumina::Get<0>(Includes).ForEachDense(Func);
            }
            else
            {
                // The probes go in by value too, so the predicate carries its pages rather than a pointer.
                Lumina::Get<0>(Includes).ForEachDenseWhere(
                    [Probes = MakeExcludeProbes()](FEntity Entity) { return !Probes.Rejects(Entity); },
                    Func);
            }
        }

        // The driver's element is already addressed by the loop index, so it never pays for a lookup.
        template<size_t Slot>
        NODISCARD FORCEINLINE auto* GatherOne(FEntity Entity, uint32 DriverDenseIndex) const
            requires CDataComponent<TIncludeAt<Slot>>
        {
            const auto& Storage = Lumina::Get<Slot>(Includes);
            if (Slot == DriverSlot)
            {
                return &Storage.GetAtDense(DriverDenseIndex);
            }
            return Storage.TryGet(Entity);
        }

        // An empty slot contributes an empty tuple, so one tuple_cat drops the tags from the argument list.
        template<size_t Slot>
        NODISCARD FORCEINLINE auto GatherSlot(FEntity Entity, uint32 DriverDenseIndex) const
        {
            if constexpr (CDataComponent<TIncludeAt<Slot>>)
            {
                return TTuple<TIncludeAt<Slot>*>(GatherOne<Slot>(Entity, DriverDenseIndex));
            }
            else
            {
                return TTuple<>{};
            }
        }

        template<size_t... Slots>
        NODISCARD FORCEINLINE auto Gather(FEntity Entity, uint32 DriverDenseIndex, std::index_sequence<Slots...>) const
        {
            return TupleCat(GatherSlot<Slots>(Entity, DriverDenseIndex)...);
        }

        // Membership for the tag includes, which the gather above deliberately leaves out.
        template<size_t... Slots>
        NODISCARD FORCEINLINE bool TagsPresent(FEntity Entity, std::index_sequence<Slots...>) const
        {
            return ((CDataComponent<TIncludeAt<Slots>> || Lumina::Get<Slots>(Includes).Contains(Entity)) && ... && true);
        }

        template<typename TFunc>
        FORCEINLINE void ForEachFiltered(TFunc Func) const
        {
            ForEachInRange(0, Driver->GetDenseSize(), Func);
        }

        TTuple<TComponentStorage<TInclude>...> Includes{};
        const FSparseSet* Excludes[ExcludeCount > 0 ? ExcludeCount : 1] = {};
        const FSparseSet* Driver = nullptr;
        size_t DriverSlot = 0;
    };

    // Type-erased iteration for tools and script, where the component types are not known at compile time.
    class FRuntimeView
    {
    public:

        void AddInclude(const FSparseSet& Set)
        {
            Includes.push_back(&Set);
            if (Driver == nullptr || Set.Num() < Driver->Num())
            {
                Driver = &Set;
            }
        }

        void AddExclude(const FSparseSet& Set) { Excludes.push_back(&Set); }

        NODISCARD const FSparseSet* GetDriver() const { return Driver; }

        // The driver bounds the result, so this is an upper bound rather than a count.
        NODISCARD size_t Num() const { return Driver != nullptr ? Driver->Num() : 0; }

        // Range-for support, so a call site can walk entities without a callback.
        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const FRuntimeView* InView, size_t InIndex)
                : View(InView)
                , Index(InIndex)
            {
                Advance();
            }

            NODISCARD FEntity operator * () const { return View->GetDriver()->GetDenseData()[Index]; }

            FIterator& operator ++ () { ++Index; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return Index == Other.Index; }
            NODISCARD bool operator != (const FIterator& Other) const { return Index != Other.Index; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FSparseSet* Set = View->GetDriver();
                const size_t Count = Set->GetDenseSize();
                const FEntity* Dense = Set->GetDenseData();

                while (Index < Count && (Dense[Index].IsTombstone() || !View->Contains(Dense[Index])))
                {
                    ++Index;
                }
            }

            const FRuntimeView* View = nullptr;
            size_t Index = 0;
        };

        NODISCARD FIterator begin() const { return FIterator(this, 0); }
        NODISCARD FIterator end() const { return FIterator(this, GetDriver() != nullptr ? GetDriver()->GetDenseSize() : 0); }

        NODISCARD bool IsEmpty() const { return Driver == nullptr || Driver->Num() == 0; }

        NODISCARD bool Contains(FEntity Entity) const
        {
            for (const FSparseSet* Set : Includes)
            {
                if (!Set->Contains(Entity))
                {
                    return false;
                }
            }
            for (const FSparseSet* Set : Excludes)
            {
                if (Set->Contains(Entity))
                {
                    return false;
                }
            }
            return true;
        }

        template<typename TFunc>
        FORCEINLINE void ForEach(TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            const FEntity* DenseData = Driver->GetDenseData();
            const size_t DenseSize = Driver->GetDenseSize();

            for (size_t Index = 0; Index < DenseSize; ++Index)
            {
                const FEntity Entity = DenseData[Index];
                if (Entity.IsTombstone() || !MatchesRest(Entity))
                {
                    continue;
                }
                Func(Entity);
            }
        }

    private:

        NODISCARD bool MatchesRest(FEntity Entity) const
        {
            for (const FSparseSet* Set : Includes)
            {
                if (Set != Driver && !Set->Contains(Entity))
                {
                    return false;
                }
            }
            for (const FSparseSet* Set : Excludes)
            {
                if (Set->Contains(Entity))
                {
                    return false;
                }
            }
            return true;
        }

        TFixedVector<const FSparseSet*, 8> Includes;
        TFixedVector<const FSparseSet*, 4> Excludes;
        const FSparseSet* Driver = nullptr;
    };
}
