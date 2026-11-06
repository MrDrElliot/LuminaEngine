#pragma once

#include "Registry.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Systems/SystemParam.h"

namespace Lumina::ECS
{
    namespace QueryInternal
    {
        template<typename T> inline constexpr bool bIsFilter  = bIsFilterTerm<T>;
        template<typename T> inline constexpr bool bIsAdded   = bIsAddedTerm<T>;

        template<typename T> struct TFilterPredicate { static constexpr bool value = bIsFilterTerm<T>; };

        template<typename T, bool bTerm = bIsFilterTerm<T> || bIsMembershipTerm<T> || bIsOptionalTerm<T>>
        struct TIsDataInclude { static constexpr bool value = CDataComponent<TBareComponent<T>>; };
        template<typename T> struct TIsDataInclude<T, true> { static constexpr bool value = false; };

        // The terms that become callback arguments, in order: data includes as references, optionals as pointers.
        template<typename T> struct TArgPredicate { static constexpr bool value = bIsOptionalTerm<T> || TIsDataInclude<T>::value; };
        template<typename T> struct TOptionalPredicate { static constexpr bool value = bIsOptionalTerm<T>; };

        template<typename T> using TOptionalComponent = TBareComponent<typename T::Spelling>;
        template<typename T> using TOptionalPointer   = std::conditional_t<std::is_const_v<TViewType<typename T::Spelling>>, const TOptionalComponent<T>*, TOptionalComponent<T>*>;

        template<typename... TArgs>
        constexpr size_t CountBefore(size_t Index, bool bOptional)
        {
            constexpr bool Flags[] = { bIsOptionalTerm<TArgs>..., false };
            size_t Count = 0;
            for (size_t Arg = 0; Arg < Index; ++Arg)
            {
                Count += Flags[Arg] == bOptional ? 1 : 0;
            }
            return Count;
        }

        template<typename TFunc, typename TArgList, typename TOptionalList>
        struct TOptionalAdapter;

        // Sits between a view walk and the caller's functor, inserting each optional's pointer at its term's position.
        template<typename TFunc, typename... TArgs, typename... TOptionals>
        struct TOptionalAdapter<TFunc, TTypeList<TArgs...>, TTypeList<TOptionals...>>
        {
            TFunc Func;
            TTuple<TComponentStorage<TOptionalComponent<TOptionals>>...> Storages;

            template<typename... TValues>
            FORCEINLINE auto operator()(FEntity Entity, TValues&... Values) -> void
            {
                TTuple<TValues&...> Data(Values...);
                Invoke(Entity, Data, std::index_sequence_for<TArgs...>{});
            }

        private:

            template<typename TData, size_t... Indices>
            FORCEINLINE void Invoke(FEntity Entity, TData& Data, std::index_sequence<Indices...>)
            {
                InvokeEntityCallback(Func, Entity, ArgAt<Indices, TArgs>(Entity, Data)...);
            }

            template<size_t Index, typename TArg, typename TData>
            FORCEINLINE decltype(auto) ArgAt(FEntity Entity, TData& Data) const
            {
                constexpr size_t Slot = CountBefore<TArgs...>(Index, bIsOptionalTerm<TArg>);
                if constexpr (bIsOptionalTerm<TArg>)
                {
                    const auto& Storage = Lumina::Get<Slot>(Storages);
                    if (!Storage.IsValid())
                    {
                        return static_cast<TOptionalPointer<TArg>>(nullptr);
                    }
                    if constexpr (TComponentTraits<TOptionalComponent<TArg>>::bEmpty)
                    {
                        static TOptionalComponent<TArg> Tag;
                        return static_cast<TOptionalPointer<TArg>>(Storage.Contains(Entity) ? &Tag : nullptr);
                    }
                    else if constexpr (std::is_const_v<std::remove_pointer_t<TOptionalPointer<TArg>>>)
                    {
                        return static_cast<TOptionalPointer<TArg>>(Storage.TryGetUntracked(Entity));
                    }
                    else
                    {
                        return static_cast<TOptionalPointer<TArg>>(Storage.TryGet(Entity));
                    }
                }
                else
                {
                    return static_cast<decltype(Lumina::Get<Slot>(Data))>(Lumina::Get<Slot>(Data));
                }
            }
        };

        // The component a non-filter term decides membership on: an include's bare type, or the one a With or Without names.
        template<typename T, bool bMembership = bIsMembershipTerm<T>> struct TMembershipOf { using Component = TBareComponent<T>; };
        template<typename T> struct TMembershipOf<T, true> { using Component = typename T::Component; };

        template<typename TFilter>
        NODISCARD FORCEINLINE bool PassesFilter(const FRegistry& Registry, FEntity Entity, uint32 Since)
        {
            const FSparseSet* Pool = Registry.FindStorage(GetComponentTypeID<typename TFilter::Component>());
            if (Pool == nullptr)
            {
                return false;
            }
            return bIsAdded<TFilter> ? Pool->HasAddedSince(Entity, Since) : Pool->HasChangedSince(Entity, Since);
        }

        template<typename TFirst, typename... TRest>
        struct TFilterList
        {
            using First = TFirst;

            NODISCARD static bool RestPass(const FRegistry& Registry, FEntity Entity, uint32 Since)
            {
                return (PassesFilter<TRest>(Registry, Entity, Since) && ...);
            }
        };
    }

    // A system member that declares its access by existing and walks the world it was bound to; an instance remembers its last run.
    template<typename... TTerms>
    class TQuery
    {
        using FFilters   = typename ViewTermInternal::TSelect<QueryInternal::TFilterPredicate, TTypeList<>, TTerms...>::Type;
        using FArgs      = typename ViewTermInternal::TSelect<QueryInternal::TArgPredicate, TTypeList<>, TTerms...>::Type;
        using FOptionals = typename ViewTermInternal::TSelect<QueryInternal::TOptionalPredicate, TTypeList<>, TTerms...>::Type;
        using FViewMaker = TViewOfTerms<TTerms...>;

    public:

        using FTerms = TTypeList<TTerms...>;
        using FView  = typename FViewMaker::Type;

        static constexpr bool bHasFilters   = (QueryInternal::bIsFilter<TTerms> || ...);
        static constexpr bool bHasOptionals = (bIsOptionalTerm<TTerms> || ...);

        TQuery() { RegisterSystemParam(this, GetOps()); }
        TQuery(const TQuery&) = default;
        TQuery& operator=(const TQuery&) = default;

        //~ Bound to the world of the system it belongs to.

        NODISCARD FView View() const { return View(BoundRegistry()); }
        NODISCARD bool IsEmpty() const { return IsEmpty(BoundRegistry()); }
        NODISCARD bool Contains(FEntity Entity) const { return View().Contains(Entity); }

        template<typename TFunc>
        void ForEach(TFunc Func) { ForEach(BoundRegistry(), Move(Func)); }

        const TVector<FEntity>& Collect() { return Collect(BoundRegistry()); }

        template<typename TFunc>
        void ParallelForEach(TFunc&& Func, uint32 MinRange = 64, uint32 MinToSplit = 0) { ParallelForEach(BoundRegistry(), Forward<TFunc>(Func), MinRange, MinToSplit); }

        // Random access, only to the components the query names, only as it names them, and only on an entity the query matches.
        template<typename C>
        NODISCARD auto* TryGet(FEntity Entity) const
        {
            static_assert(GrantOf<C> != 0, "TryGet reaches only the components this query names");
            using TResult = std::conditional_t<GrantOf<C> == 2, C, const C>;
            if (!Matches(BoundRegistry(), Entity))
            {
                return static_cast<TResult*>(nullptr);
            }
            if constexpr (GrantOf<C> == 2)
            {
                return BoundRegistry().template TryGet<Write<C>>(Entity);
            }
            else
            {
                return std::as_const(BoundRegistry()).template TryGet<C>(Entity);
            }
        }

        template<typename C>
        NODISCARD auto& Get(FEntity Entity) const
        {
            auto* Component = TryGet<C>(Entity);
            DEBUG_ASSERT(Component != nullptr, "Get on an entity that lacks the component");
            return *Component;
        }

        //~ Against any registry, for code outside a system.

        NODISCARD FView View(FRegistry& Registry) const
        {
            return FViewMaker::Assure(Registry);
        }

        // Never creates a pool, so HasWork can ask from any thread. Ignores the Changed and Added filters.
        NODISCARD bool IsEmpty(const FRegistry& Registry) const
        {
            return FViewMaker::Find(Registry).IsEmpty();
        }

        template<typename TFunc>
        void ForEach(FRegistry& Registry, TFunc InFunc)
        {
            const FView QueryView = View(Registry);
            auto&& Func = WithOptionals(Registry, Move(InFunc));
            if constexpr (!bHasFilters)
            {
                QueryView.ForEach(Move(Func));
            }
            else
            {
                WalkMatches(Registry, QueryView, [&](FEntity Entity) { QueryView.VisitEntity(Entity, Func); }, FFilters{});
                CompleteRun();
            }
        }

        // Splits the walk across the workers, running it inline when it is too small; a filtered query splits only its matches.
        template<typename TFunc>
        void ParallelForEach(FRegistry& Registry, TFunc&& InFunc, uint32 MinRange = 64, uint32 MinToSplit = 0)
        {
            auto&& Func = WithOptionals(Registry, InFunc);
            if constexpr (!bHasFilters)
            {
                const FView QueryView = View(Registry);
                const uint32 Num = static_cast<uint32>(QueryView.NumDenseSlots());
                if (Num <= MinRange || Num < MinToSplit)
                {
                    QueryView.ForEach(Func);
                    return;
                }
                Jobs::ParallelFor(Num, [&](const Jobs::FParallelRange& Range) { QueryView.ForEachInRange(Range.Start, Range.End, Func); }, MinRange);
            }
            else
            {
                const TVector<FEntity>& Matches = Collect(Registry);
                const FView QueryView = View(Registry);
                Jobs::ParallelForOrSerial(static_cast<uint32>(Matches.Num()), MinToSplit > MinRange ? MinToSplit : MinRange, [&](uint32 Index) { QueryView.VisitEntity(Matches[Index], Func); }, MinRange);
                CompleteRun();
            }
        }

        // The matching entities, held in the query's own scratch until its next Collect; call CompleteRun after visiting them.
        const TVector<FEntity>& Collect(FRegistry& Registry)
        {
            TVector<FEntity>& Out = MatchScratch;
            Out.Reset();
            const FView QueryView = View(Registry);
            if constexpr (!bHasFilters)
            {
                QueryView.ForEachEntity([&](FEntity Entity) { Out.Add(Entity); });
            }
            else
            {
                WalkMatches(Registry, QueryView, [&](FEntity Entity) { Out.Add(Entity); }, FFilters{});
            }
            return Out;
        }

        // After the visits, so their writes carry this run's tick and are not reported back to the next one.
        void CompleteRun()
        {
            if constexpr (bHasFilters)
            {
                LastRunTick = FSparseSet::AdvanceChangeTick();
            }
        }

        void Reset() { LastRunTick = 0; }

        // What a system holding this query reads and writes, as its Declare op records it.
        static void DeclareAccess(FSystemAccess& Access)
        {
            const int32 Membership = DeclareMembership(Access);
            (DeclareTerm<TTerms>(Access, Membership), ...);
        }


    private:

        template<typename TFunc>
        NODISCARD static decltype(auto) WithOptionals(FRegistry& Registry, TFunc&& Func)
        {
            if constexpr (!bHasOptionals)
            {
                return Forward<TFunc>(Func);
            }
            else
            {
                return MakeOptionalAdapter(Registry, Forward<TFunc>(Func), FOptionals{});
            }
        }

        template<typename TFunc, typename... TOptionals>
        NODISCARD static auto MakeOptionalAdapter(FRegistry& Registry, TFunc&& Func, TTypeList<TOptionals...>)
        {
            using FAdapter = QueryInternal::TOptionalAdapter<TFunc, FArgs, FOptionals>;
            return FAdapter{ Forward<TFunc>(Func), decltype(FAdapter::Storages)(Registry.template FindStorage<QueryInternal::TOptionalComponent<TOptionals>>()...) };
        }

        template<typename TOnMatch, typename... TFilterTerms>
        void WalkMatches(FRegistry& Registry, const FView& QueryView, TOnMatch&& OnMatch, TTypeList<TFilterTerms...>)
        {
            using FFilterList = QueryInternal::TFilterList<TFilterTerms...>;
            using FFirst      = typename FFilterList::First;

            (EnsureTracked<typename TFilterTerms::Component>(Registry), ...);

            const uint32 Since = LastRunTick;
            const FSparseSet* Pool = Registry.FindStorage(GetComponentTypeID<typename FFirst::Component>());
            if (Pool != nullptr)
            {
                const uint32* Ticks  = QueryInternal::bIsAdded<FFirst> ? Pool->GetAddedTickData() : Pool->GetChangeTickData();
                const FEntity* Dense = Pool->GetDenseData();
                for (int32 Index = Pool->GetDenseSize(); Index-- > 0;)
                {
                    const FEntity Entity = Dense[Index];
                    if (Entity.IsTombstone() || (Ticks != nullptr && !FSparseSet::IsNewerTick(Ticks[Index], Since)))
                    {
                        continue;
                    }
                    if (FFilterList::RestPass(Registry, Entity, Since) && QueryView.Contains(Entity))
                    {
                        OnMatch(Entity);
                    }
                }
            }
        }

        template<typename T>
        static void EnsureTracked(FRegistry& Registry)
        {
            const FSparseSet* Pool = Registry.FindStorage(GetComponentTypeID<T>());
            if (Pool != nullptr && !Pool->IsTrackingChanges())
            {
                Registry.TrackChanges<T>();
            }
        }

        // Holds every include and With, and no Without; Changed and Added only order the walk, so they never decide membership.
        NODISCARD static bool Matches(const FRegistry& Registry, FEntity Entity)
        {
            return (TermMatches<TTerms>(Registry, Entity) && ...);
        }

        template<typename TTerm>
        NODISCARD static bool TermMatches(const FRegistry& Registry, FEntity Entity)
        {
            if constexpr (QueryInternal::bIsFilter<TTerm> || bIsOptionalTerm<TTerm>)
            {
                return true;
            }
            else
            {
                const FSparseSet* Pool = Registry.FindStorage(GetComponentTypeID<typename QueryInternal::TMembershipOf<TTerm>::Component>());
                const bool bHeld = Pool != nullptr && Pool->Contains(Entity);
                return bIsWithoutTerm<TTerm> ? !bHeld : bHeld;
            }
        }

        static int32 DeclareMembership(FSystemAccess& Access)
        {
            uint32 With[sizeof...(TTerms)] = {};
            uint32 Without[sizeof...(TTerms)] = {};
            int32 NumWith = 0;
            int32 NumWithout = 0;
            ([&]
            {
                if constexpr (!QueryInternal::bIsFilter<TTerms> && !bIsOptionalTerm<TTerms>)
                {
                    const uint32 Id = static_cast<uint32>(GetComponentTypeID<typename QueryInternal::TMembershipOf<TTerms>::Component>());
                    bIsWithoutTerm<TTerms> ? (void)(Without[NumWithout++] = Id) : (void)(With[NumWith++] = Id);
                }
            }(), ...);
            return DeclareParamMembership(Access, With, NumWith, Without, NumWithout);
        }

        // A component that reaches other entities is touched past the query's own, so its access is declared unscoped.
        template<typename TTerm>
        static constexpr int32 ScopeOf(int32 QueryMembership)
        {
            if constexpr (QueryInternal::bIsFilter<TTerm>)
            {
                return QueryMembership;
            }
            else if constexpr (bIsOptionalTerm<TTerm>)
            {
                return Constants::kIndexNone;
            }
            else
            {
                using TComponent = typename QueryInternal::TMembershipOf<TTerm>::Component;
                return TComponentTraits<TComponent>::bReachesOtherEntities ? Constants::kIndexNone : QueryMembership;
            }
        }

        template<typename C, typename TTerm>
        static constexpr int32 TermGrant()
        {
            if constexpr (bIsOptionalTerm<TTerm>)
            {
                return TermGrant<C, typename TTerm::Spelling>();
            }
            else if constexpr (bIsAccessWrapper<TTerm>)
            {
                return std::is_same_v<typename TTerm::Component, C> ? (TTerm::bWrite ? 2 : 1) : 0;
            }
            else if constexpr (QueryInternal::bIsFilter<TTerm>)
            {
                return std::is_same_v<typename TTerm::Component, C> ? 1 : 0;
            }
            else if constexpr (bIsMembershipTerm<TTerm>)
            {
                return 0;
            }
            else if constexpr (std::is_const_v<TTerm>)
            {
                return std::is_same_v<std::remove_const_t<TTerm>, C> ? 1 : 0;
            }
            else
            {
                return std::is_same_v<TTerm, C> ? 2 : 0;
            }
        }

        template<typename C>
        static constexpr int32 GrantOf = [] { int32 Best = 0; ((Best = TermGrant<C, TTerms>() > Best ? TermGrant<C, TTerms>() : Best), ...); return Best; }();

        template<typename TTerm>
        static void DeclareTerm(FSystemAccess& Access, int32 QueryMembership)
        {
            const int32 Membership = ScopeOf<TTerm>(QueryMembership);
            if constexpr (bIsOptionalTerm<TTerm>)
            {
                DeclareTerm<typename TTerm::Spelling>(Access, Membership);
            }
            else if constexpr (bIsAccessWrapper<TTerm>)
            {
                const uint32 Id = static_cast<uint32>(GetComponentTypeID<typename TTerm::Component>());
                TTerm::bWrite ? DeclareParamWrite(Access, Id, true, Membership) : DeclareParamRead(Access, Id, true, Membership);
            }
            else if constexpr (QueryInternal::bIsFilter<TTerm>)
            {
                // The walk reads the whole pool's ticks, past the query's own entities, so it stays unfiltered.
                const uint32 Id = static_cast<uint32>(GetComponentTypeID<typename TTerm::Component>());
                DeclareParamRead(Access, Id, true);
                DeclareParamTracked(Access, Id);
            }
            else if constexpr (std::is_const_v<TTerm>)
            {
                DeclareParamRead(Access, static_cast<uint32>(GetComponentTypeID<std::remove_const_t<TTerm>>()), true, Membership);
            }
            else if constexpr (bIsMembershipTerm<TTerm>)
            {
                DeclareParamPool(Access, static_cast<uint32>(GetComponentTypeID<typename TTerm::Component>()));
            }
            else
            {
                DeclareParamWrite(Access, static_cast<uint32>(GetComponentTypeID<TTerm>()), true, Membership);
            }
        }

        TVector<FEntity> MatchScratch;
        uint32 LastRunTick = 0;
        FRegistry* Bound = nullptr;

        NODISCARD FRegistry& BoundRegistry() const
        {
            DEBUG_ASSERT(Bound != nullptr, "A query is bound to a world only as a member of an entity system");
            return *Bound;
        }

        // A function-local table, so it is built only where a system constructs the query and every component it names is complete.
        static const FSystemParamOps& GetOps()
        {
            static const FSystemParamOps Ops
            {
                .Bind    = [](void* Param, CWorld& World) { static_cast<TQuery*>(Param)->Bound = &GetSystemRegistry(World); },
                .Declare = [](const void*, FSystemAccess& Access) { DeclareAccess(Access); },
                .HasWork = [](const void* Param) { return !static_cast<const TQuery*>(Param)->IsEmpty(); },
            };
            return Ops;
        }
    };

    template<typename T> inline constexpr bool bIsQuery                  = false;
    template<typename... Ts> inline constexpr bool bIsQuery<TQuery<Ts...>> = true;
}
