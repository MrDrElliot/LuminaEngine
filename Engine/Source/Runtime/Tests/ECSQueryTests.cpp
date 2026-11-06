#include <atomic>
#include <csignal>
#include <gtest/gtest.h>

#include "World/ECS/Query.h"

using namespace Lumina;

namespace ECSQueryTestsInternal
{
#if defined(LE_PLATFORM_WINDOWS)
    // Windows reports abort and the crash handler as exit codes, not signals.
    inline bool Aborted(int ExitStatus) { return ExitStatus != 0; }
#else
    inline bool Aborted(int ExitStatus) { return testing::KilledBySignal(SIGABRT)(ExitStatus); }
#endif

    struct FQueryPosition
    {
        int32 Value = 0;
    };

    struct FQueryVelocity
    {
        int32 Value = 0;
    };

    struct FQueryDisabled {};

    using FMovers = ECS::TQuery<ECS::Write<FQueryPosition>, ECS::Read<FQueryVelocity>, ECS::Changed<FQueryVelocity>>;
    using FFresh  = ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Added<FQueryPosition>>;
    using FActive = ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Without<FQueryDisabled>>;
    using FMoving = ECS::TQuery<ECS::Read<FQueryPosition>, ECS::With<FQueryVelocity>, ECS::Without<FQueryDisabled>>;

    static_assert(std::is_same_v<FMovers::FView, ECS::TView<ECS::TExclude<>, ECS::Write<FQueryPosition>, const FQueryVelocity>>);
    static_assert(std::is_same_v<FActive::FView, ECS::TView<ECS::TExclude<FQueryDisabled>, const FQueryPosition>>);
    static_assert(std::is_same_v<FMoving::FView, ECS::TView<ECS::TExclude<ECS::With<FQueryVelocity>, FQueryDisabled>, const FQueryPosition>>);

    ECS::FEntity Spawn(ECS::FRegistry& Registry, int32 Velocity)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<FQueryPosition>(Entity);
        Registry.Emplace<FQueryVelocity>(Entity).Value = Velocity;
        return Entity;
    }

    int32 RunMovers(ECS::FRegistry& Registry, FMovers& Movers)
    {
        int32 Visited = 0;
        Movers.ForEach(Registry, [&](ECS::TMut<FQueryPosition> Position, const FQueryVelocity& Velocity)
        {
            Position->Value += Velocity.Value;
            ++Visited;
        });
        return Visited;
    }
}

using namespace ECSQueryTestsInternal;

TEST(ECSQuery, VariadicGetBindsEveryComponent)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Spawn(Registry, 3);

    auto [Position, Velocity] = Registry.Get<ECS::Write<FQueryPosition>, ECS::Read<FQueryVelocity>>(Entity);
    static_assert(std::is_same_v<decltype(Position), FQueryPosition&>);
    static_assert(std::is_same_v<decltype(Velocity), const FQueryVelocity&>);
    Position.Value = Velocity.Value;
    EXPECT_EQ(Registry.Get<ECS::Read<FQueryPosition>>(Entity).Value, 3);

    auto [MaybePosition, MaybeDisabled] = Registry.TryGet<ECS::Read<FQueryPosition>, ECS::Read<FQueryVelocity>>(ECS::FEntity{});
    EXPECT_EQ(MaybePosition, nullptr);
    EXPECT_EQ(MaybeDisabled, nullptr);

    const auto View = Registry.View<FQueryPosition, const FQueryVelocity>();
    auto [ViewPosition, ViewVelocity] = View.Get<FQueryPosition, FQueryVelocity>(Entity);
    static_assert(std::is_same_v<decltype(ViewVelocity), const FQueryVelocity&>, "a const include stays const through a variadic Get");
    EXPECT_EQ(ViewPosition.Value + ViewVelocity.Value, 6);
}

TEST(ECSQuery, ChangedReportsOnlyNewWritesAndNotItsOwn)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FQueryVelocity>();
    const ECS::FEntity A = Spawn(Registry, 1);
    const ECS::FEntity B = Spawn(Registry, 2);

    FMovers Movers;
    EXPECT_EQ(RunMovers(Registry, Movers), 2) << "the first run reports everything";
    EXPECT_EQ(RunMovers(Registry, Movers), 0) << "nothing changed, and its own position writes do not count";

    Registry.Get<ECS::Write<FQueryVelocity>>(B).Value = 5;
    EXPECT_EQ(RunMovers(Registry, Movers), 1);
    EXPECT_EQ(Registry.Get<ECS::Read<FQueryPosition>>(B).Value, 7);
    EXPECT_EQ(Registry.Get<ECS::Read<FQueryPosition>>(A).Value, 1);

    (void)Registry.Get<ECS::Read<FQueryVelocity>>(A);
    EXPECT_EQ(RunMovers(Registry, Movers), 0) << "a read is not a change";

    Movers.Reset();
    EXPECT_EQ(RunMovers(Registry, Movers), 2);
}

TEST(ECSQuery, ChangedTurnsTrackingOnByItself)
{
    ECS::FRegistry Registry;
    Spawn(Registry, 1);

    FMovers Movers;
    EXPECT_EQ(RunMovers(Registry, Movers), 1);
    EXPECT_TRUE(Registry.FindStorage(ECS::GetComponentTypeID<FQueryVelocity>())->IsTrackingChanges());
    EXPECT_EQ(RunMovers(Registry, Movers), 0);
}

TEST(ECSQuery, AddedReportsOnlyNewComponents)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FQueryPosition>();
    const ECS::FEntity Old = Spawn(Registry, 0);

    FFresh Fresh;
    int32 Seen = 0;
    Fresh.ForEach(Registry, [&](const FQueryPosition&) { ++Seen; });
    EXPECT_EQ(Seen, 1);

    Registry.Get<ECS::Write<FQueryPosition>>(Old).Value = 9;
    Spawn(Registry, 0);

    Seen = 0;
    Fresh.ForEach(Registry, [&](const FQueryPosition&) { ++Seen; });
    EXPECT_EQ(Seen, 1) << "a write to an old component is a change, not an add";
}

TEST(ECSQuery, WithoutExcludesAndUnfilteredQueriesVisitAll)
{
    ECS::FRegistry Registry;
    Spawn(Registry, 0);
    const ECS::FEntity Disabled = Spawn(Registry, 0);
    Registry.Emplace<FQueryDisabled>(Disabled);

    FActive Active;
    int32 Seen = 0;
    Active.ForEach(Registry, [&](ECS::FEntity Entity, const FQueryPosition&)
    {
        EXPECT_NE(Entity, Disabled);
        ++Seen;
    });
    EXPECT_EQ(Seen, 1);
}

TEST(ECSQuery, WithRequiresAComponentWithoutHandingItOver)
{
    ECS::FRegistry Registry;
    FMoving Moving;
    EXPECT_TRUE(Moving.IsEmpty(Registry)) << "a With on a missing pool matches nothing";

    const ECS::FEntity Mover = Spawn(Registry, 3);
    const ECS::FEntity Disabled = Spawn(Registry, 3);
    Registry.Emplace<FQueryDisabled>(Disabled);
    const ECS::FEntity Still = Registry.Create();
    Registry.Emplace<FQueryPosition>(Still);

    int32 Seen = 0;
    Moving.ForEach(Registry, [&](ECS::FEntity Entity, const FQueryPosition&)
    {
        EXPECT_EQ(Entity, Mover);
        ++Seen;
    });
    EXPECT_EQ(Seen, 1);
    EXPECT_TRUE(Moving.View(Registry).Contains(Mover));
    EXPECT_FALSE(Moving.View(Registry).Contains(Still));
    EXPECT_FALSE(Moving.View(Registry).Contains(Disabled));

    int32 Ranged = 0;
    const FMoving::FView View = Moving.View(Registry);
    View.ForEachInRange(0, View.NumDenseSlots(), [&](ECS::FEntity Entity, const FQueryPosition&) { EXPECT_EQ(Entity, Mover); ++Ranged; });
    EXPECT_EQ(Ranged, 1) << "the parallel slice applies With too";

    Registry.Remove<FQueryVelocity>(Mover);
    Registry.Remove<FQueryVelocity>(Disabled);
    EXPECT_TRUE(Moving.IsEmpty(Registry)) << "an empty With pool leaves the view empty, so HasWork skips the system";
}

TEST(ECSQuery, OptionalHandsOverAPointerWithoutChangingMembership)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Moving = Spawn(Registry, 4);
    const ECS::FEntity Still = Registry.Create();
    Registry.Emplace<FQueryPosition>(Still);

    using FOptionalMovers = ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Optional<FQueryVelocity>>;
    static_assert(std::is_same_v<FOptionalMovers::FView, ECS::TView<ECS::TExclude<>, const FQueryPosition>>, "an optional term never joins the view");

    FOptionalMovers Query;
    int32 Present = 0;
    int32 Absent = 0;
    Query.ForEach(Registry, [&](ECS::FEntity Entity, const FQueryPosition&, FQueryVelocity* Velocity)
    {
        if (Velocity != nullptr)
        {
            EXPECT_EQ(Entity, Moving);
            EXPECT_EQ(Velocity->Value, 4);
            Velocity->Value = 6;
            ++Present;
        }
        else
        {
            EXPECT_EQ(Entity, Still);
            ++Absent;
        }
    });
    EXPECT_EQ(Present, 1);
    EXPECT_EQ(Absent, 1);
    EXPECT_EQ(Registry.Get<ECS::Read<FQueryVelocity>>(Moving).Value, 6) << "a mutable optional writes through";

    const TVector<ECS::FEntity>& Matches = Query.Collect(Registry);
    EXPECT_EQ(Matches.Num(), 2) << "an optional term never filters";
    EXPECT_TRUE(Matches.Contains(Still));
}

TEST(ECSQuery, OptionalIsNullWhenThePoolDoesNotExist)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FQueryPosition>(Entity).Value = 2;

    ECS::TQuery<ECS::Optional<ECS::Read<FQueryVelocity>>, ECS::Write<FQueryPosition>> Query;
    int32 Seen = 0;
    Query.ForEach(Registry, [&](const FQueryVelocity* Velocity, ECS::TMut<FQueryPosition> Position)
    {
        EXPECT_EQ(Velocity, nullptr);
        Position->Value += 1;
        ++Seen;
    });
    EXPECT_EQ(Seen, 1);
    EXPECT_EQ(Registry.Get<ECS::Read<FQueryPosition>>(Entity).Value, 3) << "arguments keep their term order around the optional";
    EXPECT_EQ(Registry.FindStorage(ECS::GetComponentTypeID<FQueryVelocity>()), nullptr) << "an optional never creates a pool";
    EXPECT_FALSE(Query.IsEmpty(Registry)) << "an absent optional pool never empties the query, so HasWork still runs it";
}

TEST(ECSQuery, OptionalConstAndReadSpellingsHandOverConstPointers)
{
    ECS::FRegistry Registry;
    Spawn(Registry, 5);

    ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Optional<const FQueryVelocity>, ECS::Optional<ECS::Read<FQueryDisabled>>, ECS::Optional<ECS::Write<FQueryVelocity>>> Query;
    int32 Seen = 0;
    Query.ForEach(Registry, [&](const FQueryPosition&, auto* Const, auto* Read, auto* Write)
    {
        static_assert(std::is_same_v<decltype(Const), const FQueryVelocity*>);
        static_assert(std::is_same_v<decltype(Read), const FQueryDisabled*>);
        static_assert(std::is_same_v<decltype(Write), FQueryVelocity*>);
        ASSERT_NE(Const, nullptr);
        EXPECT_EQ(Const->Value, 5);
        EXPECT_EQ(Read, nullptr);
        EXPECT_EQ(Write, Const);
        ++Seen;
    });
    EXPECT_EQ(Seen, 1);

    const ECS::FEntity Tagged = Spawn(Registry, 7);
    Registry.Emplace<FQueryDisabled>(Tagged);
    int32 TagsSeen = 0;
    Query.ForEach(Registry, [&](ECS::FEntity Entity, const FQueryPosition&, const FQueryVelocity*, const FQueryDisabled* Tag, FQueryVelocity*)
    {
        EXPECT_EQ(Tag != nullptr, Entity == Tagged) << "a tag optional is non-null exactly when the entity holds the tag";
        ++TagsSeen;
    });
    EXPECT_EQ(TagsSeen, 2);
}

TEST(ECSQuery, OptionalWorksInParallelAndFilteredWalks)
{
    ECS::FRegistry Registry;
    constexpr int32 Count = 1000;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<FQueryPosition>(Entity).Value = Index;
        if (Index % 2 == 0)
        {
            Registry.Emplace<FQueryVelocity>(Entity).Value = 1;
        }
    }

    ECS::TQuery<ECS::Write<FQueryPosition>, ECS::Optional<ECS::Read<FQueryVelocity>>> Query;
    std::atomic<int32> WithVelocity = 0;
    std::atomic<int32> Visited = 0;
    Query.ParallelForEach(Registry, [&](const FQueryPosition& Position, const FQueryVelocity* Velocity)
    {
        if (Velocity != nullptr)
        {
            EXPECT_EQ(Position.Value % 2, 0);
            WithVelocity.fetch_add(1, std::memory_order_relaxed);
        }
        Visited.fetch_add(1, std::memory_order_relaxed);
    }, 16);
    EXPECT_EQ(Visited.load(), Count);
    EXPECT_EQ(WithVelocity.load(), Count / 2);

    Registry.TrackChanges<FQueryPosition>();
    ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Changed<FQueryPosition>, ECS::Optional<ECS::Read<FQueryVelocity>>> Changed;
    int32 ChangedWithVelocity = 0;
    int32 ChangedVisited = 0;
    Changed.ForEach(Registry, [&](const FQueryPosition&, const FQueryVelocity* Velocity)
    {
        ChangedWithVelocity += Velocity != nullptr ? 1 : 0;
        ++ChangedVisited;
    });
    EXPECT_EQ(ChangedVisited, Count);
    EXPECT_EQ(ChangedWithVelocity, Count / 2);

    ECS::TQuery<ECS::Read<FQueryPosition>, ECS::Changed<FQueryPosition>, ECS::Optional<ECS::Read<FQueryVelocity>>> ParallelChanged;
    std::atomic<int32> FilteredVisited = 0;
    std::atomic<int32> FilteredWithVelocity = 0;
    ParallelChanged.ParallelForEach(Registry, [&](const FQueryPosition&, const FQueryVelocity* Velocity)
    {
        FilteredWithVelocity.fetch_add(Velocity != nullptr ? 1 : 0, std::memory_order_relaxed);
        FilteredVisited.fetch_add(1, std::memory_order_relaxed);
    }, 16);
    EXPECT_EQ(FilteredVisited.load(), Count);
    EXPECT_EQ(FilteredWithVelocity.load(), Count / 2);
}

TEST(ECSQuery, ViewLookupsRejectEntitiesOutsideTheViewInEveryBuild)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    ECS::FRegistry Registry;
    const ECS::FEntity Moving = Spawn(Registry, 2);
    const ECS::FEntity Disabled = Spawn(Registry, 5);
    Registry.Emplace<FQueryDisabled>(Disabled);
    const ECS::FEntity Still = Registry.Create();
    Registry.Emplace<FQueryPosition>(Still);

    FActive Active;
    const auto Filtered = Active.View(Registry);
    EXPECT_TRUE(Filtered.Contains(Moving));
    EXPECT_FALSE(Filtered.Contains(Disabled));
    EXPECT_NE(Filtered.TryGet<FQueryPosition>(Moving), nullptr);
    EXPECT_EQ(Filtered.TryGet<FQueryPosition>(Disabled), nullptr);
    EXPECT_EXIT((void)Filtered.Get<FQueryPosition>(Disabled), Aborted, "");
    EXPECT_EXIT((void)Filtered.GetAll(Disabled), Aborted, "");

    const auto Joined = Registry.View<FQueryPosition, const FQueryVelocity>();
    EXPECT_EQ(Joined.TryGet<FQueryPosition>(Still), nullptr);
    EXPECT_EXIT((void)Joined.Get<FQueryPosition>(Still), Aborted, "");
    EXPECT_EXIT(([&] { auto [Position, Velocity] = Joined.Get<FQueryPosition, FQueryVelocity>(Still); (void)Position; (void)Velocity; }()), Aborted, "");
    EXPECT_EQ(Joined.Get<FQueryVelocity>(Moving).Value, 2);
}
