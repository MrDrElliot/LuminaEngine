#pragma once

#include "ComponentStorage.h"
#include "Containers/Span.h"
#include "Containers/Tuple.h"
#include "Containers/Vector.h"

#include <utility>

namespace Lumina::ECS
{
    // Membership filters: a plain T excludes holders of T and With<T> keeps only them; the registry's View<A, B>(TExclude<C>{}) spells plain excludes only.
    template<typename... Ts>
    struct TExclude {};

    // Typed multi-component iteration. The pack drives which storages it holds, nothing else.
    template<typename TFilterList, typename... TInclude>
    class TView;

    template<typename... TFilter, typename... TInclude>
    class TView<TExclude<TFilter...>, TInclude...>
    {
        static_assert(sizeof...(TInclude) > 0, "A view needs at least one included component.");
        static_assert(sizeof...(TInclude) <= 32, "A view marks the includes it stamps in a 32-bit mask.");
        static_assert((CViewComponent<TInclude> && ...), "A view includes component types or their const spelling, not references or pointers.");
        static_assert((CComponent<typename TMembershipFilter<TFilter>::Component> && ...), "A view filters on component types, not references or pointers.");
        static_assert(AreAllDistinct<TBareComponent<TInclude>...>(), "A view lists each included component once.");
        static_assert(AreAllDistinct<typename TMembershipFilter<TFilter>::Component...>(), "A view lists each filtered component once.");
        static_assert(!(bIsOneOf<TBareComponent<TInclude>, typename TMembershipFilter<TFilter>::Component...> || ...),
            "A view cannot both include and filter on the same component: an exclude never matches, a With repeats the include.");

        // Zero for a type the view does not include, so a rejected Get never names an out-of-range slot.
        template<typename T>
        NODISCARD static consteval size_t SlotOf()
        {
            constexpr bool Matches[] = { std::is_same_v<TBareComponent<T>, TBareComponent<TInclude>>... };
            for (size_t Slot = 0; Slot < sizeof...(TInclude); ++Slot)
            {
                if (Matches[Slot])
                {
                    return Slot;
                }
            }
            return 0;
        }

    public:

        static constexpr size_t IncludeCount = sizeof...(TInclude);
        static constexpr size_t FilterCount = sizeof...(TFilter);

        // As spelled, so a const include hands its callback a const reference.
        template<size_t Slot>
        using TIncludeAt = TTupleElementT<Slot, TTuple<TInclude...>>;

        template<size_t Slot>
        using TBareAt = TBareComponent<TIncludeAt<Slot>>;

        // A Write include walks as a TMut handle; every other include walks as its reference, const for a read.
        template<size_t Slot>
        static constexpr bool bHandleAt = bIsWriteAccess<TIncludeAt<Slot>>;

        // Const when either the include or the request is, so a read-only spelling (const T or Read<T>) never hands out a mutable reference.
        template<typename T>
        using TAccess = std::conditional_t<std::is_const_v<TViewType<T>> || std::is_const_v<TIncludeAt<SlotOf<T>()>>,
            const TBareComponent<T>, TBareComponent<T>>;

        // Tags carry membership only, so the callback sees the data components and nothing else.
        static constexpr size_t DataCount = ((CDataComponent<TBareComponent<TInclude>> ? 1u : 0u) + ... + 0u);

        // One included pool means the dense walk already addresses every element, filter or not.
        static constexpr bool bIsDirectScan = (IncludeCount == 1);

    private:

        // The probes are loop-invariant, so a scan pays for the page arrays once rather than per element.
        static constexpr bool bRequiredAt[] = { TMembershipFilter<TFilter>::bRequired..., false };

        struct FFilterProbes
        {
            FMembershipProbe Probes[FilterCount > 0 ? FilterCount : 1];

            // A default probe holds no entity, so a With on a missing pool rejects everything.
            NODISCARD FORCEINLINE bool Rejects(FEntity Entity) const
            {
                for (size_t Index = 0; Index < FilterCount; ++Index)
                {
                    if (Probes[Index].Contains(Entity) != bRequiredAt[Index])
                    {
                        return true;
                    }
                }
                return false;
            }
        };

    public:

        TView() = default;

        explicit TView(TComponentStorage<TBareComponent<TInclude>>... InIncludes)
            : Includes(InIncludes...)
        {
            PickDriver();
            BindStamps(std::index_sequence_for<TInclude...>{});
        }

        // Separate from the constructor because one signature cannot carry two parameter packs.
        template<typename... TSets>
        void BindFilters(TSets*... Sets)
        {
            [[maybe_unused]] size_t Slot = 0;
            ((Filters[Slot++] = static_cast<const FSparseSet*>(Sets)), ...);

            // Nothing can match a With whose pool is missing or empty, so the view reports empty and HasWork skips its system.
            for (size_t Index = 0; Index < FilterCount; ++Index)
            {
                if (bRequiredAt[Index] && (Filters[Index] == nullptr || Filters[Index]->Num() == 0))
                {
                    Driver = nullptr;
                }
            }
        }

        NODISCARD FORCEINLINE bool IsEmpty() const { return Driver == nullptr || Driver->Num() == 0; }

        // Upper bound only. The driver is the smallest included pool, so the real count is at most this.
        NODISCARD FORCEINLINE int32 NumCandidates() const { return Driver != nullptr ? Driver->Num() : 0; }

        // Dense slots, not live entities, because splitting on the live count drops a tombstoned tail.
        NODISCARD FORCEINLINE int32 NumDenseSlots() const { return Driver == nullptr ? 0 : (Group != nullptr ? Group->Num : Driver->GetDenseSize()); }

        // Whether the includes are exactly one owning group's types, so walks index every pool at the same dense slot.
        NODISCARD FORCEINLINE bool IsGrouped() const { return Group != nullptr && Driver != nullptr; }

        // The highest entity index the driver holds, which bounds every entity the view can match.
        NODISCARD FORCEINLINE uint32 MaxLiveIndex() const { return Driver != nullptr ? Driver->MaxLiveIndex() : 0u; }

        // Every dense slot including tombstones, offered only where the single unfiltered pool is exactly the view's membership.
        NODISCARD FORCEINLINE TSpan<const FEntity> GetDenseEntities() const requires (bIsDirectScan && FilterCount == 0)
        {
            return Driver != nullptr ? TSpan<const FEntity>(Driver->GetDenseData(), Driver->GetDenseSize()) : TSpan<const FEntity>();
        }

        NODISCARD FORCEINLINE bool IsDrivenBy(const FSparseSet* Set) const { return Driver != nullptr && Driver == Set; }

        // The driver bounds the result, so this is an upper bound rather than a count.
        NODISCARD FORCEINLINE int32 Num() const { return NumCandidates(); }

        // Whether a mutable Get on this view stamps change ticks, which a mutable include of a tracked pool does. Walks never stamp.
        NODISCARD FORCEINLINE bool StampsChanges() const { return StampMask != 0; }

        // Range-for support, so a call site can walk entities without a callback. Backward, like ForEach.
        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const TView* InView, int32 InEndSlot)
                : View(InView)
                , EndSlot(InEndSlot)
            {
                if (InView != nullptr && InEndSlot > 0)
                {
                    Probes = InView->MakeFilterProbes();
                }
                Advance();
            }

            NODISCARD FEntity operator * () const { return View->GetDriver()->GetDenseData()[EndSlot - 1]; }

            FIterator& operator ++ () { --EndSlot; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return EndSlot == Other.EndSlot; }
            NODISCARD bool operator != (const FIterator& Other) const { return EndSlot != Other.EndSlot; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FEntity* Dense = View->GetDriver()->GetDenseData();
                while (EndSlot > 0 && (Dense[EndSlot - 1].IsTombstone() || !View->MatchesRestWith(Dense[EndSlot - 1], Probes)))
                {
                    --EndSlot;
                }
            }

            const TView* View = nullptr;

            // One past the current slot, so the finished walk sits at zero without wrapping.
            int32 EndSlot = 0;
            FFilterProbes Probes{};
        };

        NODISCARD FIterator begin() const { return FIterator(this, NumDenseSlots()); }
        NODISCARD FIterator end() const { return FIterator(this, 0); }

        // Tuple iteration, so a range-for can bind the entity and its components and still use break.
        class FEachIterator
        {
        public:

            FEachIterator() = default;

            FEachIterator(const TView* InView, int32 InEndSlot)
                : View(InView)
                , EndSlot(InEndSlot)
            {
                if (InView != nullptr && InEndSlot > 0)
                {
                    Probes = InView->MakeFilterProbes();
                }
                Advance();
            }

            NODISCARD auto operator * () const
            {
                const int32 Index = EndSlot - 1;
                const FEntity Entity = View->GetDriver()->GetDenseData()[Index];
                uint32 DenseSlots[IncludeCount];
                (void)View->FindDenseSlots(Entity, static_cast<uint32>(Index), DenseSlots, std::index_sequence_for<TInclude...>{});
                return TupleCat(TTuple<FEntity>(Entity), View->ArgsAt(DenseSlots, TickSink, std::index_sequence_for<TInclude...>{}));
            }

            FEachIterator& operator ++ () { --EndSlot; Advance(); return *this; }

            NODISCARD bool operator == (const FEachIterator& Other) const { return EndSlot == Other.EndSlot; }
            NODISCARD bool operator != (const FEachIterator& Other) const { return EndSlot != Other.EndSlot; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FEntity* Dense = View->GetDriver()->GetDenseData();
                while (EndSlot > 0 && (Dense[EndSlot - 1].IsTombstone() || !View->MatchesRestWith(Dense[EndSlot - 1], Probes)))
                {
                    --EndSlot;
                }
            }

            const TView* View = nullptr;

            // One past the current slot, so the finished walk sits at zero without wrapping.
            int32 EndSlot = 0;
            FFilterProbes Probes{};

            // Where handles into an untracked pool stamp, owned by the walk so parallel walks never share a line.
            mutable uint32 TickSink = 0;
        };

        struct FEachRange
        {
            const TView* View = nullptr;

            NODISCARD FEachIterator begin() const { return FEachIterator(View, View->NumDenseSlots()); }
            NODISCARD FEachIterator end() const { return FEachIterator(View, 0); }
        };

        NODISCARD FEachRange Each() const { return FEachRange{ this }; }

        // Every included data component at once, so a caller can Apply over the pack.
        NODISCARD auto GetAll(FEntity Entity) const
        {
            CheckMembership(Entity);
            uint32 DenseSlots[IncludeCount];
            (void)FindDenseSlots(Entity, Driver->GetDenseIndex(Entity), DenseSlots, std::index_sequence_for<TInclude...>{});
            return ArgsAt(DenseSlots, GetAllTickSink, std::index_sequence_for<TInclude...>{});
        }

        template<typename T>
            requires (bIsOneOf<TBareComponent<T>, TBareComponent<TInclude>...> && CDataComponent<TBareComponent<T>>)
        NODISCARD FORCEINLINE TAccess<T>& Get(FEntity Entity) const
        {
            CheckMembership(Entity);
            return GetMatched<T>(Entity);
        }

        // Null for an entity the view does not match, and for every entity when the view has no pool for the component.
        template<typename T>
            requires (bIsOneOf<TBareComponent<T>, TBareComponent<TInclude>...> && CDataComponent<TBareComponent<T>>)
        NODISCARD FORCEINLINE TAccess<T>* TryGet(FEntity Entity) const
        {
            constexpr size_t Slot = SlotOf<T>();
            const auto& Storage = Lumina::Get<Slot>(Includes);
            if (!Storage.IsValid() || Entity.IsNull())
            {
                return nullptr;
            }

            // Only an entity the whole view matches, so a lookup never reaches past the query's filters into another system's entities.
            if constexpr (IncludeCount > 1 || FilterCount > 0)
            {
                if (!Contains(Entity))
                {
                    return nullptr;
                }
            }
            const FEntity Found = Storage.GetSet()->FindSlot(Entity.GetIndex());
            if (Found.GetVersion() != Entity.GetVersion())
            {
                return nullptr;
            }
            if constexpr (!std::is_const_v<TAccess<T>>)
            {
                StampAt<Slot>(Found.GetIndex());
            }
            return &Storage.GetAtDense(Found.GetIndex());
        }

        // Several includes at once, for structured bindings, with the same const rules as the single Get.
        template<typename T0, typename T1, typename... TRest>
            requires (((bIsOneOf<TBareComponent<TRest>, TBareComponent<TInclude>...> && CDataComponent<TBareComponent<TRest>>) && ... && true)
                && bIsOneOf<TBareComponent<T0>, TBareComponent<TInclude>...> && CDataComponent<TBareComponent<T0>>
                && bIsOneOf<TBareComponent<T1>, TBareComponent<TInclude>...> && CDataComponent<TBareComponent<T1>>)
        NODISCARD FORCEINLINE auto Get(FEntity Entity) const
        {
            CheckMembership(Entity);
            return TTuple<TAccess<T0>&, TAccess<T1>&, TAccess<TRest>&...>(GetMatched<T0>(Entity), GetMatched<T1>(Entity), GetMatched<TRest>(Entity)...);
        }

        template<typename T0, typename T1, typename... TRest>
        NODISCARD FORCEINLINE auto TryGet(FEntity Entity) const
        {
            return TTuple<TAccess<T0>*, TAccess<T1>*, TAccess<TRest>*...>(TryGet<T0>(Entity), TryGet<T1>(Entity), TryGet<TRest>(Entity)...);
        }

        // One entity, handed over like a walk would; false when the entity does not match the view.
        template<typename TFunc>
        bool VisitEntity(FEntity Entity, TFunc&& Func) const
        {
            uint32 DenseSlots[IncludeCount];
            if (Driver == nullptr || FailsFilters(Entity)
                || !FindDenseSlots(Entity, Driver->FindLiveDense(Entity), DenseSlots, std::index_sequence_for<TInclude...>{}))
            {
                return false;
            }
            uint32 TickSink = 0;
            auto Args = ArgsAt(DenseSlots, TickSink, std::index_sequence_for<TInclude...>{});
            Apply([&Func, Entity](auto&... Values) { InvokeEntityCallback(Func, Entity, Values...); }, Args);
            return true;
        }

        NODISCARD bool Contains(FEntity Entity) const
        {
            if (Driver == nullptr)
            {
                return false;
            }
            return MatchesIncludes(Entity) && !FailsFilters(Entity);
        }

        // By value all the way down, so the functor's captures stay in registers across the scan.
        template<typename TFunc>
        FORCEINLINE void ForEach(TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            if constexpr (!bIsDirectScan)
            {
                if (Group != nullptr)
                {
                    ForEachGroupedRange(0, Group->Num, std::move(Func));
                    return;
                }
            }

            // A write handle needs its row's dense slot, which only the indexed walk carries.
            if constexpr (bIsDirectScan && !bHandleAt<0>)
            {
                ForEachDirect(std::move(Func));
            }
            else
            {
                ForEachFiltered(std::move(Func));
            }
        }

        // A slice of the driver's dense array for one parallel job, so split on NumDenseSlots().
        template<typename TFunc>
        FORCEINLINE void ForEachInRange(int32 Begin, int32 End, TFunc Func) const
        {
            if (Driver == nullptr)
            {
                return;
            }

            if constexpr (!bIsDirectScan)
            {
                if (Group != nullptr)
                {
                    ForEachGroupedRange(Begin, End, std::move(Func));
                    return;
                }
            }

            const FEntity* DenseData = Driver->GetDenseData();
            const int32 DenseSize = Driver->GetDenseSize();
            const uint8* DriverPackedBlock = Driver->GetPackedBlock();
            const int32 Last = End < DenseSize ? End : DenseSize;
            const FFilterProbes Probes = MakeFilterProbes();
            uint32 TickSink = 0;

            // Backward, so removing the current entity only pulls an already visited one into its slot.
            for (int32 Index = Last; Index-- > Begin;)
            {
                const FEntity Entity = DenseData[Index];
                if (Entity.IsTombstone())
                {
                    continue;
                }

                if constexpr (FilterCount > 0)
                {
                    if (Probes.Rejects(Entity))
                    {
                        continue;
                    }
                }

                // One lookup per pool, reused for the callback argument, so membership is never tested twice.
                uint32 DenseSlots[IncludeCount];
                if (!FindDenseSlots(Entity, static_cast<uint32>(Index), DenseSlots, std::index_sequence_for<TInclude...>{}))
                {
                    continue;
                }

                auto Args = ArgsAt(DenseSlots, TickSink, std::index_sequence_for<TInclude...>{});
                Apply([&Func, Entity](auto&... Value) { InvokeEntityCallback(Func, Entity, Value...); }, Args);
            }

            CheckWalkDidNotReallocate(Driver, DenseData, DriverPackedBlock);
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
            const int32 DenseSize = Driver->GetDenseSize();
            const uint8* DriverPackedBlock = Driver->GetPackedBlock();

            if (Group != nullptr)
            {
                const FFilterProbes Probes = MakeFilterProbes();
                for (int32 Index = Group->Num; Index-- > 0;)
                {
                    const FEntity Entity = DenseData[Index];
                    if constexpr (FilterCount > 0)
                    {
                        if (Probes.Rejects(Entity))
                        {
                            continue;
                        }
                    }
                    Func(Entity);
                }
                return;
            }

            // A packed single pool has nothing to filter and no holes, so the walk is just the array.
            constexpr bool bUnfiltered = (IncludeCount == 1 && FilterCount == 0);
            if (bUnfiltered && !Driver->HasTombstones())
            {
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    Func(DenseData[Index]);
                }
            }
            else
            {
                const FFilterProbes Probes = MakeFilterProbes();
                for (int32 Index = DenseSize; Index-- > 0;)
                {
                    const FEntity Entity = DenseData[Index];
                    if (Entity.IsTombstone() || !MatchesRestWith(Entity, Probes))
                    {
                        continue;
                    }
                    Func(Entity);
                }
            }

            CheckWalkDidNotReallocate(Driver, DenseData, DriverPackedBlock);
        }

    private:

        NODISCARD FORCEINLINE const FSparseSet* GetDriver() const { return Driver; }

        FORCEINLINE void CheckMembership(FEntity Entity) const
        {
            if constexpr (IncludeCount > 1 || FilterCount > 0)
            {
                ASSERT(Contains(Entity), "Get on an entity the view does not match, which reaches past what its query declared");
            }
        }

        template<typename T>
        NODISCARD FORCEINLINE TAccess<T>& GetMatched(FEntity Entity) const
        {
            constexpr size_t Slot = SlotOf<T>();
            const auto& Storage = Lumina::Get<Slot>(Includes);
            DEBUG_ASSERT(Storage.IsValid() && Storage.Contains(Entity), "Get on a component the entity does not have");
            DEBUG_ASSERT(Contains(Entity), "Get on an entity the view does not match, which reaches past what its query declared");
            const uint32 DenseIndex = Storage.GetSet()->GetDenseIndex(Entity);
            if constexpr (!std::is_const_v<TAccess<T>>)
            {
                StampAt<Slot>(DenseIndex);
            }
            return Storage.GetAtDense(DenseIndex);
        }

        // A missing pool means the view can never match, so it stays empty rather than driving off another.
        void PickDriver()
        {
            Driver = nullptr;
            DriverSlot = 0;

            const bool bAnyMissing = Apply([](const auto&... Storage) { return (!Storage.IsValid() || ...); }, Includes);
            if (bAnyMissing)
            {
                return;
            }

            size_t Slot = 0;
            Apply([this, &Slot](const auto&... Storage) { (ConsiderDriver(Storage.GetSet(), Slot++), ...); }, Includes);

            if constexpr (IncludeCount > 1)
            {
                const FOwningGroup* Candidate = Lumina::Get<0>(Includes).GetSet()->GetOwningGroup();
                const bool bAllOwned = Candidate != nullptr && Candidate->Owned.Num() == static_cast<int32>(IncludeCount)
                    && Apply([Candidate](const auto&... Storage) { return ((Storage.GetSet()->GetOwningGroup() == Candidate) && ...); }, Includes);
                if (bAllOwned)
                {
                    Group = Candidate;
                    Driver = Lumina::Get<0>(Includes).GetSet();
                    DriverSlot = 0;
                }
            }
        }

        // Every include shares the dense slot, so each element is addressed by the walk index and nothing is probed but the filters.
        template<typename TFunc>
        FORCEINLINE void ForEachGroupedRange(int32 Begin, int32 End, TFunc Func) const
        {
            const FEntity* DenseData = Driver->GetDenseData();
            const uint8* DriverPackedBlock = Driver->GetPackedBlock();
            const int32 Last = End < Group->Num ? End : Group->Num;
            const FFilterProbes Probes = MakeFilterProbes();
            const auto Bases = Apply([](const auto&... Storage) { return TTuple<decltype(GroupBase(Storage))...>(GroupBase(Storage)...); }, Includes);
            uint32 TickSink = 0;

            for (int32 Index = Last; Index-- > Begin;)
            {
                const FEntity Entity = DenseData[Index];
                if constexpr (FilterCount > 0)
                {
                    if (Probes.Rejects(Entity))
                    {
                        continue;
                    }
                }

                InvokeGrouped(Func, Entity, Bases, Index, TickSink, std::index_sequence_for<TInclude...>{});
            }

            CheckWalkDidNotReallocate(Driver, DenseData, DriverPackedBlock);
        }

        // A packed pool's block, so the grouped walk indexes it directly; a paged pool or a tag keeps its storage.
        template<typename TStorage>
        NODISCARD static FORCEINLINE auto GroupBase(const TStorage& Storage)
        {
            using T = typename TStorage::ValueType;
            if constexpr (CDataComponent<T> && !TStorage::bPaged)
            {
                return reinterpret_cast<T*>(Storage.GetSet()->GetPackedBlock());
            }
            else
            {
                return Storage;
            }
        }

        template<size_t Slot, typename TBase>
        NODISCARD FORCEINLINE auto GroupedSlot(const TBase& Base, int32 Index, uint32& TickSink) const
        {
            if constexpr (!CDataComponent<TBareAt<Slot>>)
            {
                return TTuple<>{};
            }
            else if constexpr (std::is_pointer_v<TBase>)
            {
                return ArgFor<Slot>(Base[Index], static_cast<uint32>(Index), TickSink);
            }
            else
            {
                return ArgFor<Slot>(Base.GetAtDense(static_cast<uint32>(Index)), static_cast<uint32>(Index), TickSink);
            }
        }

        template<typename TFunc, typename TBases, size_t... Slots>
        FORCEINLINE void InvokeGrouped(TFunc& Func, FEntity Entity, const TBases& Bases, int32 Index, uint32& TickSink, std::index_sequence<Slots...>) const
        {
            auto Args = TupleCat(GroupedSlot<Slots>(Lumina::Get<Slots>(Bases), Index, TickSink)...);
            Apply([&Func, Entity](auto&... Value) { InvokeEntityCallback(Func, Entity, Value...); }, Args);
        }

        // Dense slots rather than live count, since a stable pool walks its tombstones too; an empty pool ends the scan outright.
        NODISCARD static int32 ScanCost(const FSparseSet* Set)
        {
            return Set->Num() == 0 ? 0 : Set->GetDenseSize();
        }

        void ConsiderDriver(const FSparseSet* Candidate, size_t Slot)
        {
            if (Driver == nullptr || ScanCost(Candidate) < ScanCost(Driver))
            {
                Driver = Candidate;
                DriverSlot = Slot;
            }
        }

        // The epoch moves now, since the view hands out mutable access from here on; a write handle takes the tick even before its pool is tracked.
        template<size_t... Slots>
        void BindStamps(std::index_sequence<Slots...>)
        {
            StampMask = ((WantsStamp<Slots>() ? (1u << Slots) : 0u) | ... | 0u);
            if (StampMask != 0 || (bHandleAt<Slots> || ...))
            {
                StampTick = FSparseSet::GetChangeTick();
                (NoteStampSlot<Slots>(), ...);
            }
        }

        template<size_t Slot>
        NODISCARD bool WantsStamp() const
        {
            if constexpr (std::is_const_v<TIncludeAt<Slot>> || !CDataComponent<TBareAt<Slot>>)
            {
                return false;
            }
            else
            {
                const FSparseSet* Set = Lumina::Get<Slot>(Includes).GetSet();
                return Set != nullptr && Set->IsTrackingChanges();
            }
        }

        template<size_t Slot>
        void NoteStampSlot() const
        {
            if ((StampMask & (1u << Slot)) != 0)
            {
                Lumina::Get<Slot>(Includes).GetSet()->NoteStampTick(StampTick);
            }
        }

        template<size_t Slot>
        FORCEINLINE void StampAt(uint32 DenseIndex) const
        {
            if ((StampMask & (1u << Slot)) != 0)
            {
                Lumina::Get<Slot>(Includes).GetSet()->SetChangeTickAt(DenseIndex, StampTick);
            }
        }

        NODISCARD FORCEINLINE bool FailsFilters(FEntity Entity) const
        {
            if constexpr (FilterCount == 0)
            {
                return false;
            }
            else
            {
                for (size_t Index = 0; Index < FilterCount; ++Index)
                {
                    const bool bHeld = Filters[Index] != nullptr && Filters[Index]->Contains(Entity);
                    if (bHeld != bRequiredAt[Index])
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        NODISCARD FFilterProbes MakeFilterProbes() const
        {
            FFilterProbes Result;
            for (size_t Index = 0; Index < FilterCount; ++Index)
            {
                if (Filters[Index] != nullptr)
                {
                    Result.Probes[Index] = Filters[Index]->MakeProbe();
                }
            }
            return Result;
        }

        // Every include's dense slot, the driver's taken from the walk; false when any pool lacks the entity.
        template<size_t... Slots>
        NODISCARD FORCEINLINE bool FindDenseSlots(FEntity Entity, uint32 DriverDenseIndex, uint32 (&Out)[IncludeCount], std::index_sequence<Slots...>) const
        {
            ((Out[Slots] = (Slots == DriverSlot ? DriverDenseIndex : Lumina::Get<Slots>(Includes).GetSet()->FindLiveDense(Entity))), ...);
            return ((Out[Slots] != FSparseSet::InvalidDenseIndex) && ... && true);
        }

        template<size_t Slot>
        NODISCARD FORCEINLINE auto ArgFor(TBareAt<Slot>& Value, uint32 DenseIndex, uint32& TickSink) const
        {
            if constexpr (bHandleAt<Slot>)
            {
                using FHandle = TMut<TBareAt<Slot>>;
                return TTuple<FHandle>(FHandle(Value, Lumina::Get<Slot>(Includes).GetSet()->GetChangeTickSlot(DenseIndex, TickSink), StampTick));
            }
            else
            {
                return TTuple<TViewType<TIncludeAt<Slot>>&>(Value);
            }
        }

        // A tag carries no value, so it never reaches the callback or the tuple the iterator hands back.
        template<size_t Slot>
        NODISCARD FORCEINLINE auto ArgAt(uint32 DenseIndex, uint32& TickSink) const
        {
            if constexpr (CDataComponent<TBareAt<Slot>>)
            {
                return ArgFor<Slot>(Lumina::Get<Slot>(Includes).GetAtDense(DenseIndex), DenseIndex, TickSink);
            }
            else
            {
                return TTuple<>{};
            }
        }

        template<size_t... Slots>
        NODISCARD FORCEINLINE auto ArgsAt(const uint32 (&DenseSlots)[IncludeCount], uint32& TickSink, std::index_sequence<Slots...>) const
        {
            return TupleCat(ArgAt<Slots>(DenseSlots[Slots], TickSink)...);
        }

        NODISCARD FORCEINLINE bool MatchesIncludes(FEntity Entity) const
        {
            return Apply([Entity](const auto&... Storage)
            {
                return ((Storage.IsValid() && Storage.Contains(Entity)) && ...);
            }, Includes);
        }

        // Everything except the driver's own membership, which the dense walk already established.
        NODISCARD FORCEINLINE bool MatchesNonDriverIncludes(FEntity Entity) const
        {
            if constexpr (IncludeCount > 1)
            {
                return Apply([this, Entity](const auto&... Storage)
                {
                    return ((Storage.GetSet() == Driver || Storage.Contains(Entity)) && ...);
                }, Includes);
            }
            else
            {
                return true;
            }
        }

        // Takes the probes the caller already hoisted, so a scan reloads no pool pointer per element.
        NODISCARD FORCEINLINE bool MatchesRestWith(FEntity Entity, const FFilterProbes& Probes) const
        {
            return MatchesNonDriverIncludes(Entity) && !Probes.Rejects(Entity);
        }

        // A const include hands the callback a const reference, so a read-only view cannot write through the scan.
        template<typename TFunc>
        FORCEINLINE void ForEachDirect(TFunc Func) const
        {
            if constexpr (std::is_const_v<TIncludeAt<0>> && CDataComponent<TBareAt<0>>)
            {
                ForEachDirectScan([Inner = std::move(Func)](FEntity Entity, TBareAt<0>& Value) mutable
                {
                    InvokeEntityCallback(Inner, Entity, std::as_const(Value));
                });
            }
            else
            {
                ForEachDirectScan(std::move(Func));
            }
        }

        template<typename TFunc>
        FORCEINLINE void ForEachDirectScan(TFunc Func) const
        {
            if constexpr (FilterCount == 0)
            {
                Lumina::Get<0>(Includes).ForEachDense(std::move(Func));
            }
            else
            {
                // The probes go in by value too, so the predicate carries its pages rather than a pointer.
                Lumina::Get<0>(Includes).ForEachDenseWhere(
                    [Probes = MakeFilterProbes()](FEntity Entity) { return !Probes.Rejects(Entity); },
                    std::move(Func));
            }
        }

        template<typename TFunc>
        FORCEINLINE void ForEachFiltered(TFunc Func) const
        {
            ForEachInRange(0, Driver->GetDenseSize(), std::move(Func));
        }

        TTuple<TComponentStorage<TBareComponent<TInclude>>...> Includes{};
        const FSparseSet* Filters[FilterCount > 0 ? FilterCount : 1] = {};
        const FSparseSet* Driver = nullptr;
        size_t DriverSlot = 0;

        // Set when the includes are exactly this group's owned types; its Num is read at walk time, so a view outlives membership changes.
        const FOwningGroup* Group = nullptr;

        // One bit per include slot this view stamps, set only for a mutable include of a tracked pool.
        uint32 StampMask = 0;
        uint32 StampTick = 0;

        // The handles GetAll returns outlive the call, so their untracked sink lives with the view.
        mutable uint32 GetAllTickSink = 0;
    };

    // Type-erased iteration for tools and script, where the component types are not known at compile time.
    class FRuntimeView
    {
    public:

        // By dense slots, as TView picks, since the walk pays for tombstones too and an empty pool ends it outright.
        void AddInclude(const FSparseSet& Set)
        {
            Includes.Add(&Set);
            if (Driver == nullptr || ScanCost(Set) < ScanCost(*Driver))
            {
                Driver = &Set;
            }
        }

        void AddExclude(const FSparseSet& Set) { Excludes.Add(&Set); }

        NODISCARD bool IsDrivenBy(const FSparseSet* Set) const { return Driver != nullptr && Driver == Set; }

        // The driver bounds the result, so this is an upper bound rather than a count.
        NODISCARD int32 Num() const { return Driver != nullptr ? Driver->Num() : 0; }

        // Range-for support, so a call site can walk entities without a callback. Backward, like ForEach.
        class FIterator
        {
        public:

            FIterator() = default;

            FIterator(const FRuntimeView* InView, int32 InEndSlot)
                : View(InView)
                , EndSlot(InEndSlot)
            {
                Advance();
            }

            NODISCARD FEntity operator * () const { return View->GetDriver()->GetDenseData()[EndSlot - 1]; }

            FIterator& operator ++ () { --EndSlot; Advance(); return *this; }

            NODISCARD bool operator == (const FIterator& Other) const { return EndSlot == Other.EndSlot; }
            NODISCARD bool operator != (const FIterator& Other) const { return EndSlot != Other.EndSlot; }

        private:

            void Advance()
            {
                if (View == nullptr || View->GetDriver() == nullptr)
                {
                    return;
                }

                const FEntity* Dense = View->GetDriver()->GetDenseData();
                while (EndSlot > 0 && (Dense[EndSlot - 1].IsTombstone() || !View->MatchesRest(Dense[EndSlot - 1])))
                {
                    --EndSlot;
                }
            }

            const FRuntimeView* View = nullptr;

            // One past the current slot, so the finished walk sits at zero without wrapping.
            int32 EndSlot = 0;
        };

        NODISCARD FIterator begin() const { return FIterator(this, GetDriver() != nullptr ? GetDriver()->GetDenseSize() : 0); }
        NODISCARD FIterator end() const { return FIterator(this, 0); }

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
            const int32 DenseSize = Driver->GetDenseSize();
            const uint8* DriverPackedBlock = Driver->GetPackedBlock();

            for (int32 Index = DenseSize; Index-- > 0;)
            {
                const FEntity Entity = DenseData[Index];
                if (Entity.IsTombstone() || !MatchesRest(Entity))
                {
                    continue;
                }
                Func(Entity);
            }

            CheckWalkDidNotReallocate(Driver, DenseData, DriverPackedBlock);
        }

    private:

        NODISCARD const FSparseSet* GetDriver() const { return Driver; }

        NODISCARD static int32 ScanCost(const FSparseSet& Set)
        {
            return Set.Num() == 0 ? 0 : Set.GetDenseSize();
        }

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

        TInlineVector<const FSparseSet*, 8> Includes;
        TInlineVector<const FSparseSet*, 4> Excludes;
        const FSparseSet* Driver = nullptr;
    };

    template<typename... Ts>
    struct TTypeList {};

    namespace ViewTermInternal
    {
        template<typename TList, typename T, bool bKeep>
        struct TAppendIf;

        template<typename... Ts, typename T>
        struct TAppendIf<TTypeList<Ts...>, T, true> { using Type = TTypeList<Ts..., T>; };

        template<typename... Ts, typename T>
        struct TAppendIf<TTypeList<Ts...>, T, false> { using Type = TTypeList<Ts...>; };

        template<template<typename> class TPredicate, typename TList, typename... Ts>
        struct TSelect { using Type = TList; };

        template<template<typename> class TPredicate, typename TList, typename T, typename... Ts>
        struct TSelect<TPredicate, TList, T, Ts...>
        {
            using Type = typename TSelect<TPredicate, typename TAppendIf<TList, T, TPredicate<T>::value>::Type, Ts...>::Type;
        };

        template<typename T> struct TIncludePredicate    { static constexpr bool value = !bIsFilterTerm<T> && !bIsMembershipTerm<T> && !bIsOptionalTerm<T>; };
        template<typename T> struct TMembershipPredicate { static constexpr bool value = bIsMembershipTerm<T>; };

        // Without<T> filters as a plain T, With<T> keeps its wrapper so the view knows to require it.
        template<typename T> struct TFilterSpelling          { using Type = typename T::Component; };
        template<typename T> struct TFilterSpelling<With<T>> { using Type = With<T>; };

        template<typename TIncludes, typename TMemberships>
        struct TBuild;

        template<typename... TIncludes, typename... TMemberships>
        struct TBuild<TTypeList<TIncludes...>, TTypeList<TMemberships...>>
        {
            using Type = TView<TExclude<typename TFilterSpelling<TMemberships>::Type...>, TViewInclude<TIncludes>...>;

            template<typename TRegistry>
            static Type Assure(TRegistry& Registry)
            {
                Type Result(Registry.template AssureStorage<TBareComponent<TIncludes>>()...);
                Result.BindFilters(Registry.template AssureStorage<typename TMemberships::Component>().GetSet()...);
                return Result;
            }

            template<typename TRegistry>
            static Type Find(const TRegistry& Registry)
            {
                Type Result(Registry.template FindStorage<TBareComponent<TIncludes>>()...);
                Result.BindFilters(Registry.template FindStorage<typename TMemberships::Component>().GetSet()...);
                return Result;
            }
        };
    }

    // Splits view terms into includes (T, const T, Read<T>, Write<T>) and With<T>/Without<T> membership filters; Changed and Added are left to TQuery.
    template<typename... TTerms>
    struct TViewOfTerms : ViewTermInternal::TBuild<
        typename ViewTermInternal::TSelect<ViewTermInternal::TIncludePredicate, TTypeList<>, TTerms...>::Type,
        typename ViewTermInternal::TSelect<ViewTermInternal::TMembershipPredicate, TTypeList<>, TTerms...>::Type>
    {
    };

    template<typename T>
    concept CViewTerm = CViewComponent<T> || bIsMembershipTerm<T>;
}
