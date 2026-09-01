#include "BenchCommon.h"

#include "World/ECS/Context.h"

namespace ECSBench
{
    // What the API refuses to compile, taking the component as a concept parameter so it fails softly.
    namespace ConstraintChecks
    {
        using ECS::FEntity;
        using ECS::FRegistry;
        using ECS::TExclude;

        template<typename T>
        concept CCanGet = requires (FRegistry& R, FEntity E) { R.template Get<T>(E); };

        template<typename T>
        concept CCanTryGet = requires (FRegistry& R, FEntity E) { R.template TryGet<T>(E); };

        template<typename T>
        concept CCanPatch = requires (FRegistry& R, FEntity E) { R.template Patch<T>(E); };

        template<typename T, typename... TArgs>
        concept CCanEmplace = requires (FRegistry& R, FEntity E)
        {
            R.template Emplace<T>(E, std::declval<TArgs>()...);
        };

        template<typename TViewType, typename T>
        concept CViewCanGet = requires (const TViewType& V, FEntity E) { V.template Get<T>(E); };

        static_assert(ECS::CDataComponent<FPosition>, "a component with fields is a data component");
        static_assert(ECS::CTagComponent<FDisabledTag>, "an empty component is a tag");
        static_assert(!ECS::CComponent<FPosition*>, "a pointer is not a component type");
        static_assert(!ECS::CComponent<FPosition&>, "a reference is not a component type");
        static_assert(!ECS::CComponent<const FPosition>, "a const spelling is not a component type");

        static_assert(ECS::AreAllDistinct<FPosition, FVelocity>(), "distinct types pass the pack check");
        static_assert(!ECS::AreAllDistinct<FPosition, FVelocity, FPosition>(), "a repeat fails the pack check");

        static_assert(CCanGet<FPosition>, "reading a data component is allowed");
        static_assert(!CCanGet<FDisabledTag>, "a tag has no value, so Get must not compile");
        static_assert(!CCanTryGet<FDisabledTag>, "a tag has no value, so TryGet must not compile");
        static_assert(!CCanPatch<FDisabledTag>, "a tag has no value to patch");
        static_assert(!CCanGet<FPosition*>, "a pointer spelling must not compile");
        static_assert(!CCanGet<const FPosition>, "a const spelling must not compile");

        static_assert(CCanEmplace<FDisabledTag>, "a tag can be added");
        static_assert(!CCanEmplace<FDisabledTag, int>, "a tag takes no constructor arguments");
        static_assert(CCanEmplace<FPosition, FPosition>, "a data component takes a value");

        static_assert(std::is_void_v<decltype(std::declval<FRegistry&>().Emplace<FDisabledTag>(FEntity{}))>,
            "adding a tag hands back nothing");

        using FTestView = decltype(std::declval<FRegistry&>().View<FPosition, FDisabledTag>());
        static_assert(CViewCanGet<FTestView, FPosition>, "a view reads an included data component");
        static_assert(!CViewCanGet<FTestView, FVelocity>, "a view cannot read a component it does not include");
        static_assert(!CViewCanGet<FTestView, FDisabledTag>, "a view cannot read an included tag");
        static_assert(FTestView::DataCount == 1, "a tag does not reach the callback argument list");
    }
}

namespace ECSBench
{
    namespace
    {
        int Failures = 0;
        int Checks = 0;

        void Check(bool bCondition, const char* What)
        {
            ++Checks;
            if (!bCondition)
            {
                ++Failures;
                std::printf("  FAIL  %s\n", What);
            }
        }

        // Every trait the surface exposes, proved at compile time rather than described in a comment.
        namespace TraitProbes
        {
            using ECS::EComponentLayout;
            using ECS::TComponentTraits;

            struct FPlain { float X = 0.0f; };
            struct FFat { float M[16] = {}; };
            struct FForcedPacked { static constexpr auto Layout = EComponentLayout::Packed; float M[16] = {}; };
            struct FForcedPaged { static constexpr auto Layout = EComponentLayout::Paged; float X = 0.0f; };
            struct FSmallPages { static constexpr uint32 PageSize = 64; float X = 0.0f; };
            struct FNewSpelling { static constexpr bool InPlaceDelete = true; float X = 0.0f; };

            static_assert(!TComponentTraits<FPlain>::InPlaceDelete, "a component defaults to swap-and-pop");
            static_assert(TComponentTraits<FPlain>::Layout == EComponentLayout::Automatic, "layout defaults to automatic");
            static_assert(TComponentTraits<FPlain>::PageSize == 1024, "page size defaults to 1024");
            static_assert(TComponentTraits<FPlain>::bPaged, "an automatic component is paged for pointer stability");

            static_assert(TComponentTraits<FFat>::bPaged, "a fat automatic component is paged");
            static_assert(!TComponentTraits<FForcedPacked>::bPaged, "an explicit packed layout opts out of paging");
            static_assert(TComponentTraits<FForcedPaged>::bPaged, "an explicit paged layout stays paged");

            static_assert(TComponentTraits<FSmallPages>::PageSize == 64, "a component sets its own page size");
            static_assert(TComponentTraits<FSmallPages>::bPaged, "an automatic component honors its own page size");

            static_assert(TComponentTraits<FNewSpelling>::InPlaceDelete, "InPlaceDelete is read");
            static_assert(TComponentTraits<FNewSpelling>::bPaged, "in-place delete implies paged");

            static_assert(!TComponentTraits<FDisabledTag>::bPaged, "a tag has no payload to page");
        }

        // Drains the tombstone chain and allocates past its end, where a truncated terminator returns IndexMask.
        void CheckTombstoneReuse()
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;
            for (uint32 Index = 0; Index < 8; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FStablePosition>(Entity, FStablePosition{ static_cast<float>(Index), 0.0f, 0.0f });
                Entities.push_back(Entity);
            }

            const ECS::TComponentStorage<FStablePosition> Pool = Registry.GetStorage<FStablePosition>();
            const size_t DenseBefore = Pool.GetDenseSize();

            Registry.Remove<FStablePosition>(Entities[2]);
            Registry.Remove<FStablePosition>(Entities[5]);
            Check(Pool.GetDenseSize() == DenseBefore, "an in-place removal leaves the dense array the same size");
            Check(Pool.Num() == DenseBefore - 2, "two removals drop the live count by two");

            // Two refills consume both tombstones; the third must append rather than follow a stale link.
            for (uint32 Extra = 0; Extra < 3; ++Extra)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FStablePosition>(Entity, FStablePosition{ 100.0f + static_cast<float>(Extra), 0.0f, 0.0f });
                Entities.push_back(Entity);
            }

            Check(Pool.GetDenseSize() == DenseBefore + 1, "refills reuse both holes and then append exactly one slot");
            Check(Pool.Num() == DenseBefore + 1, "the pool has no tombstones left");

            size_t Walked = 0;
            Registry.View<FStablePosition>().ForEach([&Walked](ECS::FEntity, FStablePosition&) { ++Walked; });
            Check(Walked == DenseBefore + 1, "iteration sees every live element after the chain drained");

            Check(Registry.Get<FStablePosition>(Entities[0]).X == 0.0f, "an untouched element keeps its value");
            Check(Registry.Get<FStablePosition>(Entities[10]).X == 102.0f, "the appended element keeps its value");
        }

        // Exercises a page size small enough that a few hundred entities span many pages.
        void CheckComponentTraits()
        {
            using namespace TraitProbes;

            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;

            constexpr uint32 Count = 500;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FSmallPages>(Entity, FSmallPages{ static_cast<float>(Index) });
                Entities.push_back(Entity);
            }

            Check(Registry.GetStorage<FSmallPages>().GetSet()->GetPayloadPageSize() == 64,
                "a custom page size reaches the pool");

            bool bAllReadBack = true;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                if (Registry.Get<FSmallPages>(Entities[Index]).X != static_cast<float>(Index))
                {
                    bAllReadBack = false;
                }
            }
            Check(bAllReadBack, "a custom page size reads back every element across page boundaries");

            for (uint32 Index = 0; Index < Count; Index += 2)
            {
                Registry.Remove<FSmallPages>(Entities[Index]);
            }

            Check(Registry.GetStorage<FSmallPages>().Num() == Count / 2,
                "removal from a custom-page-size pool tracks the count");

            bool bSurvivorsIntact = true;
            for (uint32 Index = 1; Index < Count; Index += 2)
            {
                if (!Registry.HasAll<FSmallPages>(Entities[Index])
                    || Registry.Get<FSmallPages>(Entities[Index]).X != static_cast<float>(Index))
                {
                    bSurvivorsIntact = false;
                }
            }
            Check(bSurvivorsIntact, "survivors keep their values after paged swap-and-pop");

            ECS::FRegistry Forced;
            const ECS::FEntity A = Forced.Create();
            const ECS::FEntity B = Forced.Create();

            Forced.Emplace<FForcedPaged>(A, FForcedPaged{ 1.0f });
            Forced.Emplace<FForcedPaged>(B, FForcedPaged{ 2.0f });
            Forced.Remove<FForcedPaged>(A);

            Check(Forced.GetStorage<FForcedPaged>().GetSet()->IsPaged(), "a forced paged pool is paged");
            Check(!Forced.GetStorage<FForcedPaged>().HasTombstones(),
                "a paged pool without in-place delete swaps rather than leaving a hole");
            Check(Forced.Get<FForcedPaged>(B).X == 2.0f, "the survivor of a paged swap keeps its value");

            ECS::FRegistry Packed;
            const ECS::FEntity C = Packed.Create();
            Packed.Emplace<FForcedPacked>(C);
            Check(!Packed.GetStorage<FForcedPacked>().GetSet()->IsPaged(),
                "a forced packed pool stays packed even past the size threshold");

            ECS::FRegistry Stable;
            const ECS::FEntity D = Stable.Create();
            const ECS::FEntity E = Stable.Create();
            Stable.Emplace<FNewSpelling>(D);
            Stable.Emplace<FNewSpelling>(E);
            Stable.Remove<FNewSpelling>(D);
            Check(Stable.GetStorage<FNewSpelling>().HasTombstones(),
                "InPlaceDelete leaves a tombstone rather than swapping");
        }

        // ForEachInRange indexes the dense array, so a pool with holes must still be split on dense slots.
        void CheckRangeIteration()
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;

            constexpr uint32 Count = 400;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FStablePosition>(Entity, FStablePosition{ static_cast<float>(Index), 0.0f, 0.0f });
                Entities.push_back(Entity);
            }

            // FStablePosition is in-place delete, so removal leaves tombstones and Num() drops below the dense size.
            for (uint32 Index = 0; Index < Count; Index += 2)
            {
                Registry.Remove<FStablePosition>(Entities[Index]);
            }

            auto View = Registry.View<FStablePosition>();
            Check(View.NumDenseSlots() > View.NumCandidates(),
                "a pool with tombstones has more dense slots than live entities");

            size_t WholeWalk = 0;
            View.ForEach([&WholeWalk](ECS::FEntity, FStablePosition&) { ++WholeWalk; });

            // Split the way a parallel job would, in chunks over the dense bound.
            size_t Chunked = 0;
            const size_t Slots = View.NumDenseSlots();
            for (size_t Begin = 0; Begin < Slots; Begin += 32)
            {
                const size_t End = Begin + 32 < Slots ? Begin + 32 : Slots;
                View.ForEachInRange(Begin, End, [&Chunked](ECS::FEntity, FStablePosition&) { ++Chunked; });
            }

            Check(WholeWalk == Count / 2, "the whole walk visits every surviving entity");
            Check(Chunked == WholeWalk, "chunked range iteration visits exactly what the whole walk does");

            size_t Overrun = 0;
            View.ForEachInRange(0, Slots * 4, [&Overrun](ECS::FEntity, FStablePosition&) { ++Overrun; });
            Check(Overrun == WholeWalk, "a range past the end is clamped rather than reading off the array");
        }

        void CheckEntityLifecycle()
        {
            ECS::FRegistry Registry;

            const ECS::FEntity A = Registry.Create();
            const ECS::FEntity B = Registry.Create();

            Check(Registry.IsValid(A) && Registry.IsValid(B), "fresh entities are valid");
            Check(A != B, "fresh entities differ");
            Check(Registry.NumEntities() == 2, "entity count tracks creation");

            Registry.Destroy(A);
            Check(!Registry.IsValid(A), "a destroyed entity stops being valid");
            Check(Registry.IsValid(B), "destroying one entity leaves the other alone");

            const ECS::FEntity C = Registry.Create();
            Check(C.GetIndex() == A.GetIndex(), "a freed slot is reused");
            Check(C.GetVersion() != A.GetVersion(), "a reused slot carries a new version");
            Check(!Registry.IsValid(A), "the stale handle stays invalid after reuse");
        }

        void CheckCreateWithHint()
        {
            ECS::FRegistry Registry;

            const ECS::FEntity Far = Registry.Create(ECS::FEntity(500, 3));
            Check(Far.GetIndex() == 500 && Far.GetVersion() == 3, "a hint materializes exactly");
            Check(Registry.IsValid(Far), "a hinted entity is valid");

            const ECS::FEntity Filled = Registry.Create();
            Check(Filled.GetIndex() < 500, "slots skipped by the hint are reused first");
            Check(Registry.IsValid(Filled), "a reused skipped slot is valid");

            const ECS::FEntity Taken = Registry.Create(Far);
            Check(Taken != Far, "a hint onto a live slot hands back a fresh entity");
        }

        void CheckPackedAndStable()
        {
            ECS::FRegistry Registry;

            TVector<ECS::FEntity> Entities;
            for (int Index = 0; Index < 64; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity, FPosition{ static_cast<float>(Index), 0.0f, 0.0f });
                Registry.Emplace<FStablePosition>(Entity, FStablePosition{ static_cast<float>(Index), 0.0f, 0.0f });
                Entities.push_back(Entity);
            }

            // Odd indices survive the removal below, so this is a component that outlives the churn.
            FStablePosition* Pinned = &Registry.Get<FStablePosition>(Entities[41]);

            for (int Index = 0; Index < 64; Index += 2)
            {
                Registry.Remove<FPosition>(Entities[Index]);
                Registry.Remove<FStablePosition>(Entities[Index]);
            }

            Check(&Registry.Get<FStablePosition>(Entities[41]) == Pinned, "a stable component keeps its address");
            Check(Registry.Get<FStablePosition>(Entities[41]).X == 41.0f, "a stable component keeps its value");
            Check(Registry.TryGet<FStablePosition>(Entities[40]) == nullptr, "a removed stable component is gone");
            Check(Registry.Get<FPosition>(Entities[41]).X == 41.0f, "a packed component survives a swap-and-pop");

            int Seen = 0;
            Registry.View<FPosition>().ForEach([&](ECS::FEntity, FPosition&) { ++Seen; });
            Check(Seen == 32, "a packed view visits exactly the live elements");

            Seen = 0;
            Registry.View<FStablePosition>().ForEach([&](ECS::FEntity, FStablePosition&) { ++Seen; });
            Check(Seen == 32, "a stable view skips tombstones");

            Registry.Compact();

            Seen = 0;
            Registry.View<FStablePosition>().ForEach([&](ECS::FEntity, FStablePosition& Value)
            {
                Check(Value.X == static_cast<float>(2 * Seen + 1), "compaction preserves values in order");
                ++Seen;
            });
            Check(Seen == 32, "a compacted view still visits every live element");
        }

        void CheckViewFiltering()
        {
            ECS::FRegistry Registry;

            for (int Index = 0; Index < 90; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity);
                if (Index % 2 == 0) { Registry.Emplace<FVelocity>(Entity); }
                if (Index % 3 == 0) { Registry.Emplace<FHealth>(Entity); }
                if (Index % 5 == 0) { Registry.Emplace<FDisabledTag>(Entity); }
            }

            int Both = 0;
            Registry.View<FPosition, FVelocity>().ForEach([&](ECS::FEntity, FPosition&, FVelocity&) { ++Both; });
            Check(Both == 45, "a two-component view intersects");

            int All = 0;
            Registry.View<FPosition, FVelocity, FHealth>().ForEach(
                [&](ECS::FEntity, FPosition&, FVelocity&, FHealth&) { ++All; });
            Check(All == 15, "a three-component view intersects");

            int Filtered = 0;
            Registry.View<FPosition, FVelocity>(ECS::TExclude<FDisabledTag>{}).ForEach(
                [&](ECS::FEntity, FPosition&, FVelocity&) { ++Filtered; });
            Check(Filtered == 36, "an exclude drops the tagged entities");

            int Entities = 0;
            Registry.View<FPosition, FHealth>().ForEachEntity([&](ECS::FEntity) { ++Entities; });
            Check(Entities == 30, "entity-only iteration matches the same set");
        }

        void CheckDestroyDetaches()
        {
            ECS::FRegistry Registry;

            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<FPosition>(Entity);
            Registry.Emplace<FVelocity>(Entity);
            Registry.Emplace<FDisabledTag>(Entity);

            Check(Registry.HasAll<FPosition, FVelocity, FDisabledTag>(Entity), "HasAll sees every component");
            Check(Registry.HasAny<FHealth, FVelocity>(Entity), "HasAny sees one of two");

            Registry.Destroy(Entity);

            for (ECS::FSparseSet* Storage : Registry.GetActiveStorages())
            {
                Check(!Storage->Contains(Entity), "destroying an entity detaches it from every pool");
            }
        }

        void CheckNonTrivialLifetime()
        {
            FOwning::Destroyed = 0;

            {
                ECS::FRegistry Registry;
                TVector<ECS::FEntity> Entities;

                // Enough to force several reallocations, which is where a relocation bug would show.
                for (int Index = 0; Index < 300; ++Index)
                {
                    const ECS::FEntity Entity = Registry.Create();
                    Registry.Emplace<FOwning>(Entity);
                    Entities.push_back(Entity);
                }

                bool bAllLive = true;
                for (ECS::FEntity Entity : Entities)
                {
                    const FOwning& Value = Registry.Get<FOwning>(Entity);
                    bAllLive = bAllLive && Value.Data != nullptr && *Value.Data == 7;
                }
                Check(bAllLive, "a non-trivial component survives pool growth");

                Registry.Remove<FOwning>(Entities[10]);
                Check(Registry.TryGet<FOwning>(Entities[10]) == nullptr, "removal clears the component");
                Check(Registry.Get<FOwning>(Entities[299]).Data != nullptr, "the swapped-in element stays intact");
            }

            Check(FOwning::Destroyed > 0, "a non-trivial component is destructed");
        }

        void CheckContext()
        {
            ECS::FRegistry Registry;

            Check(!Registry.HasSingleton<FCameraState>(), "an empty context holds nothing");
            Check(Registry.TryGetSingleton<FCameraState>() == nullptr, "a missing singleton is null");

            FCameraState& Camera = Registry.EmplaceSingleton<FCameraState>();
            Camera.Fov = 42.0f;

            Check(Registry.HasSingleton<FCameraState>(), "an emplaced singleton is present");
            Check(Registry.GetSingleton<FCameraState>().Fov == 42.0f, "a singleton keeps its value");
            Check(&Registry.GetSingleton<FCameraState>() == &Camera, "a singleton keeps its address");

            Registry.EmplaceSingleton<FWorldClock>(FWorldClock{ 1.5 });
            Check(Registry.GetSingleton<FWorldClock>().Seconds == 1.5, "a second singleton coexists");
            Check(Registry.GetSingleton<FCameraState>().Fov == 42.0f, "adding one singleton leaves the other alone");

            Check(Registry.EraseSingleton<FCameraState>(), "erasing a present singleton reports true");
            Check(!Registry.HasSingleton<FCameraState>(), "an erased singleton is gone");
            Check(!Registry.EraseSingleton<FCameraState>(), "erasing a missing singleton reports false");
            Check(Registry.GetSingleton<FWorldClock>().Seconds == 1.5, "erasing one singleton leaves the other alone");
        }

        void CheckNamedStorages()
        {
            ECS::FRegistry Registry;

            const ECS::FEntity A = Registry.Create();
            const ECS::FEntity B = Registry.Create();

            auto Red  = Registry.NamedStorage<FTagLabel>(FName("Red"));
            auto Blue = Registry.NamedStorage<FTagLabel>(FName("Blue"));

            Red.Emplace(A, FTagLabel{ 1 });
            Blue.Emplace(B, FTagLabel{ 2 });

            Check(Red.Contains(A) && !Red.Contains(B), "a named pool holds only its own entities");
            Check(Blue.Contains(B) && !Blue.Contains(A), "a second named pool is independent");
            Check(Red.Get(A).Value == 1 && Blue.Get(B).Value == 2, "named pools keep their own values");

            auto SameRed = Registry.NamedStorage<FTagLabel>(FName("Red"));
            Check(SameRed.GetSet() == Red.GetSet(), "the same name resolves to the same pool");

            // The unnamed pool of the same type is a third, separate one.
            Registry.Emplace<FTagLabel>(A, FTagLabel{ 3 });
            Check(Registry.Get<FTagLabel>(A).Value == 3, "the unnamed pool is separate");
            Check(Red.Get(A).Value == 1, "the named pool is untouched by the unnamed one");

            Registry.Destroy(A);
            Check(!Red.Contains(A), "destroying an entity detaches it from named pools too");
        }

        void CheckSwap()
        {
            ECS::FRegistry Left;
            ECS::FRegistry Right;

            const ECS::FEntity LeftEntity = Left.Create();
            Left.Emplace<FPosition>(LeftEntity, FPosition{ 11.0f, 0.0f, 0.0f });
            Left.EmplaceSingleton<FWorldClock>(FWorldClock{ 9.0 });

            const ECS::FEntity RightA = Right.Create();
            const ECS::FEntity RightB = Right.Create();
            Right.Emplace<FHealth>(RightA, FHealth{ 5.0f });
            Right.Emplace<FHealth>(RightB, FHealth{ 6.0f });

            Left.Swap(Right);

            Check(Left.NumEntities() == 2, "swap moves the entity count");
            Check(Right.NumEntities() == 1, "swap moves the entity count the other way");
            Check(Left.Get<FHealth>(RightA).Value == 5.0f, "swap moves component data");
            Check(Right.Get<FPosition>(LeftEntity).X == 11.0f, "swap moves component data the other way");
            Check(Right.HasSingleton<FWorldClock>(), "swap moves the context");
            Check(!Left.HasSingleton<FWorldClock>(), "swap leaves no context behind");
            Check(Right.GetSingleton<FWorldClock>().Seconds == 9.0, "a swapped singleton keeps its value");
        }

        void CheckSignals()
        {
            ECS::FRegistry Registry;

            static int Constructed = 0;
            static int Destroyed = 0;
            static int Updated = 0;
            Constructed = Destroyed = Updated = 0;

            ECS::FComponentSignals& Signals = Registry.GetSignals<FPosition>();
            Signals.OnConstruct.Connect([](void*, ECS::FRegistry&, ECS::FEntity) { ++Constructed; });
            Signals.OnDestroy.Connect([](void*, ECS::FRegistry&, ECS::FEntity) { ++Destroyed; });
            const ECS::FSignalConnection UpdateHandle =
                Signals.OnUpdate.Connect([](void*, ECS::FRegistry&, ECS::FEntity) { ++Updated; });

            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<FPosition>(Entity);
            Check(Constructed == 1, "emplace fires OnConstruct");

            Registry.Patch<FPosition>(Entity);
            Check(Updated == 1, "patch fires OnUpdate");

            Registry.Remove<FPosition>(Entity);
            Check(Destroyed == 1, "remove fires OnDestroy");

            Registry.Emplace<FPosition>(Entity);
            Registry.Destroy(Entity);
            Check(Destroyed == 2, "destroying an entity fires OnDestroy for its components");

            ECS::FSignalConnection Handle = UpdateHandle;
            Handle.Release();

            const ECS::FEntity Second = Registry.Create();
            Registry.Emplace<FPosition>(Second);
            Registry.Patch<FPosition>(Second);
            Check(Updated == 1, "a released connection stops receiving");
        }

        // Names come from the compiler's own signature when a type was never reflected.
        void CheckSignatureNames()
        {
            const ECS::FComponentTypeID PositionID = ECS::GetComponentTypeID<FPosition>();
            const ECS::FComponentTypeID VelocityID = ECS::GetComponentTypeID<FVelocity>();
            const ECS::FComponentTypeID TagID = ECS::GetComponentTypeID<FDisabledTag>();

            Check(PositionID != VelocityID && PositionID != TagID, "distinct types get distinct ids");

            ECS::FComponentTypeRegistry& Types = ECS::FComponentTypeRegistry::Get();
            const FName PositionName = Types.GetInfo(PositionID).Name;
            const FName VelocityName = Types.GetInfo(VelocityID).Name;

            Check(PositionName != VelocityName, "distinct types get distinct names");
            Check(PositionName == FName("ECSBench::FPosition"), "the derived name is the qualified type name");
            Check(Types.GetInfo(TagID).Name == FName("ECSBench::FDisabledTag"), "a tag is named the same way");
            Check(Types.GetInfo(PositionID).GetStruct() == nullptr, "an unreflected component carries no CStruct");

            Check(ECS::GetComponentTypeID<FPosition>() == PositionID, "asking twice returns the same id");
        }

        // A tag in the include list filters, and never shows up as a callback argument.
        void CheckTagInViewPack()
        {
            ECS::FRegistry Registry;

            for (int Index = 0; Index < 40; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity, FPosition{ static_cast<float>(Index), 0.0f, 0.0f });
                if (Index % 4 == 0)
                {
                    Registry.Emplace<FDisabledTag>(Entity);
                }
            }

            int Seen = 0;
            float Sum = 0.0f;
            Registry.View<FPosition, FDisabledTag>().ForEach([&](ECS::FEntity, FPosition& Position)
            {
                ++Seen;
                Sum += Position.X;
            });
            Check(Seen == 10, "a tag in the pack filters the view");
            Check(Sum == 0.0f + 4 + 8 + 12 + 16 + 20 + 24 + 28 + 32 + 36, "the filtered set is the right one");

            int TagOnly = 0;
            Registry.View<FDisabledTag>().ForEach([&](ECS::FEntity) { ++TagOnly; });
            Check(TagOnly == 10, "a tag-only view calls back with the entity alone");
        }

        void CheckRuntimeView()
        {
            ECS::FRegistry Registry;

            for (int Index = 0; Index < 60; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity);
                if (Index % 2 == 0) { Registry.Emplace<FVelocity>(Entity); }
                if (Index % 4 == 0) { Registry.Emplace<FDisabledTag>(Entity); }
            }

            ECS::FRuntimeView View;
            View.AddInclude(*Registry.FindStorage(ECS::GetComponentTypeID<FPosition>()));
            View.AddInclude(*Registry.FindStorage(ECS::GetComponentTypeID<FVelocity>()));
            View.AddExclude(*Registry.FindStorage(ECS::GetComponentTypeID<FDisabledTag>()));

            int Seen = 0;
            View.ForEach([&](ECS::FEntity) { ++Seen; });
            Check(Seen == 15, "a runtime view intersects and excludes");
        }
    }

    bool RunSelfCheck()
    {
        std::printf("self check\n");

        CheckEntityLifecycle();
        CheckCreateWithHint();
        CheckPackedAndStable();
        CheckViewFiltering();
        CheckDestroyDetaches();
        CheckNonTrivialLifetime();
        CheckContext();
        CheckNamedStorages();
        CheckSwap();
        CheckSignals();
        CheckSignatureNames();
        CheckTagInViewPack();
        CheckRuntimeView();
        CheckComponentTraits();
        CheckTombstoneReuse();
        CheckRangeIteration();

        std::printf("  %d checks, %d failures\n\n", Checks, Failures);
        return Failures == 0;
    }
}
