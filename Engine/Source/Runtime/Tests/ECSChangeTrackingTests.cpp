#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Systems/SystemAccess.h"

using namespace Lumina;

namespace ECSChangeTrackingTestsInternal
{
    struct FChangeTrackedValue
    {
        int32 Value = 0;
    };

    struct FChangeTrackedOther
    {
        int32 Value = 0;
    };

    struct FChangeTrackedStable
    {
        static constexpr bool InPlaceDelete = true;
        int32 Value = 0;
    };

    struct FChangeTrackedNeverCreated
    {
        int32 Value = 0;
    };

    struct FChangeTrackedTag {};

    void CountSignal(int32& Count, ECS::FRegistry&, ECS::FEntity)
    {
        ++Count;
    }

    template<typename T>
    TVector<ECS::FEntity> ChangedSinceOf(const ECS::FRegistry& Registry, uint32 Since)
    {
        TVector<ECS::FEntity> Changed;
        if (const ECS::FSparseSet* Set = Registry.FindStorage(ECS::GetComponentTypeID<T>()))
        {
            for (int32 Index = Set->GetDenseSize(); Index-- > 0;)
            {
                const ECS::FEntity Entity = Set->GetDenseData()[Index];
                if (!Entity.IsTombstone() && Set->HasChangedSince(Entity, Since))
                {
                    Changed.Add(Entity);
                }
            }
        }
        return Changed;
    }

    TVector<ECS::FEntity> ChangedSince(const ECS::FRegistry& Registry, uint32 Since)
    {
        return ChangedSinceOf<FChangeTrackedValue>(Registry, Since);
    }

    template<typename T>
    bool HasChangedSince(const ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Since)
    {
        const ECS::FSparseSet* Set = Registry.FindStorage(ECS::GetComponentTypeID<T>());
        return Set != nullptr && Set->HasChangedSince(Entity, Since);
    }

    using FConstView = decltype(std::declval<const ECS::FRegistry&>().FindView<const FChangeTrackedValue>());
    using FMixedView = decltype(std::declval<ECS::FRegistry&>().View<const FChangeTrackedValue, FChangeTrackedOther>());

    static_assert(std::is_same_v<decltype(std::declval<const FConstView&>().Get<FChangeTrackedValue>(ECS::FEntity{})), const FChangeTrackedValue&>,
        "a const include hands out a const reference even when asked for the bare type");
    static_assert(std::is_same_v<decltype(std::declval<const FMixedView&>().Get<FChangeTrackedOther>(ECS::FEntity{})), FChangeTrackedOther&>,
        "a mutable include in the same view stays mutable");
    static_assert(std::is_same_v<decltype(std::declval<const FMixedView&>().Get<const FChangeTrackedOther>(ECS::FEntity{})), const FChangeTrackedOther&>,
        "asking for the const spelling of a mutable include reads it const");
}

using namespace ECSChangeTrackingTestsInternal;

TEST(ECSChangeTracking, EpochMovesOnEmplaceRemovePatchAndClear)
{
    ECS::FRegistry Registry;
    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();

    Registry.Emplace<FChangeTrackedValue>(A);
    const uint64 AfterFirst = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterFirst, 0u);

    Registry.Emplace<FChangeTrackedValue>(B);
    const uint64 AfterEmplace = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterEmplace, AfterFirst);

    Registry.Patch<FChangeTrackedValue>(A, [](FChangeTrackedValue& Value) { Value.Value = 7; });
    const uint64 AfterPatch = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterPatch, AfterEmplace);

    Registry.EmplaceOrReplace<FChangeTrackedValue>(A, FChangeTrackedValue{ 9 });
    const uint64 AfterReplace = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterReplace, AfterPatch);

    Registry.PatchRaw(ECS::GetComponentTypeID<FChangeTrackedValue>(), A);
    const uint64 AfterPatchRaw = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterPatchRaw, AfterReplace);

    Registry.Remove<FChangeTrackedValue>(B);
    const uint64 AfterRemove = Registry.GetWriteEpoch<FChangeTrackedValue>();
    EXPECT_NE(AfterRemove, AfterPatchRaw);

    Registry.ClearComponent<FChangeTrackedValue>();
    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), AfterRemove);
}

TEST(ECSChangeTracking, EpochIgnoresDirectWritesAndReadOnlyViews)
{
    ECS::FRegistry Registry;
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    const uint64 Before = Registry.GetWriteEpoch<FChangeTrackedValue>();

    Registry.Get<FChangeTrackedValue>(A).Value = 3;
    (void)Registry.GetOrEmplace<FChangeTrackedValue>(A);

    int32 Seen = 0;
    Registry.View<const FChangeTrackedValue>().ForEach([&](const FChangeTrackedValue& Value) { Seen += Value.Value; });
    Registry.FindView<const FChangeTrackedValue>().ForEach([&](ECS::FEntity, const FChangeTrackedValue& Value) { Seen += Value.Value; });
    for (auto [Entity, Value] : Registry.View<const FChangeTrackedValue>().Each())
    {
        Seen += Value.Value;
    }

    EXPECT_EQ(Seen, 9);
    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedValue>(), Before) << "the epoch is documented to miss untracked writes and reads";
}

TEST(ECSChangeTracking, MissingPoolEpochIsZeroAndCreatesNothing)
{
    ECS::FRegistry Registry;
    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedNeverCreated>(), 0u);
    EXPECT_FALSE(Registry.FindStorage<FChangeTrackedNeverCreated>().IsValid());
    EXPECT_FALSE(HasChangedSince<FChangeTrackedNeverCreated>(Registry, Registry.Create(), 0));
}

TEST(ECSChangeTracking, EpochOfASwappedInPoolNeverRepeatsTheOldOne)
{
    ECS::FRegistry Live;
    ECS::FRegistry Pending;
    Live.Emplace<FChangeTrackedValue>(Live.Create());
    Pending.Emplace<FChangeTrackedValue>(Pending.Create());

    const uint64 Cached = Live.GetWriteEpoch<FChangeTrackedValue>();
    Live.Swap(Pending);

    EXPECT_NE(Live.GetWriteEpoch<FChangeTrackedValue>(), Cached);
    EXPECT_EQ(Pending.GetWriteEpoch<FChangeTrackedValue>(), Cached);
}

TEST(ECSChangeTracking, ScheduledWriteMovesOnlyTheDeclaredPools)
{
    ECS::FRegistry Registry;
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedOther>(A);

    FSystemAccess Writer;
    Writer.Write<FChangeTrackedValue>().Read<FChangeTrackedOther>();

    const uint64 WrittenBefore = Registry.GetWriteEpoch<FChangeTrackedValue>();
    const uint64 ReadBefore = Registry.GetWriteEpoch<FChangeTrackedOther>();
    MarkDeclaredWrites(Registry, Writer);

    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), WrittenBefore);
    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedOther>(), ReadBefore) << "a declared read never moves the epoch";
}

TEST(ECSChangeTracking, ScheduledWriteMovesAnUntrackedEpoch)
{
    ECS::FRegistry Registry;
    Registry.Emplace<FChangeTrackedValue>(Registry.Create());

    const uint64 Before = Registry.GetWriteEpoch<FChangeTrackedValue>();
    FSystemAccess Writer;
    Writer.Write<FChangeTrackedValue>();
    MarkDeclaredWrites(Registry, Writer);
    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), Before) << "an untracked pool cannot see its writes, so a scheduled writer moves it";
    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedNeverCreated>(), 0u);
}

TEST(ECSChangeTracking, ExclusiveSystemMovesEveryUntrackedPool)
{
    ECS::FRegistry Registry;
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedOther>(A);

    const uint64 ValueBefore = Registry.GetWriteEpoch<FChangeTrackedValue>();
    const uint64 OtherBefore = Registry.GetWriteEpoch<FChangeTrackedOther>();
    MarkDeclaredWrites(Registry, FSystemAccess::Exclusive());

    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), ValueBefore);
    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedOther>(), OtherBefore);
}

TEST(ECSChangeTracking, FindViewNeverCreatesAPool)
{
    ECS::FRegistry Registry;
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A, FChangeTrackedValue{ 4 });

    auto Missing = Registry.FindView<FChangeTrackedNeverCreated>();
    int32 Visited = 0;
    Missing.ForEach([&](ECS::FEntity, FChangeTrackedNeverCreated&) { ++Visited; });
    Missing.ForEachEntity([&](ECS::FEntity) { ++Visited; });
    for (ECS::FEntity Entity : Missing)
    {
        (void)Entity;
        ++Visited;
    }
    EXPECT_TRUE(Missing.IsEmpty());
    EXPECT_EQ(Missing.TryGet<FChangeTrackedNeverCreated>(A), nullptr);

    auto Joined = Registry.FindView<const FChangeTrackedValue, FChangeTrackedNeverCreated>();
    Joined.ForEach([&](const FChangeTrackedValue&, FChangeTrackedNeverCreated&) { ++Visited; });
    EXPECT_EQ(Visited, 0) << "a missing include leaves the view empty";

    int32 Sum = 0;
    Registry.FindView<const FChangeTrackedValue>(ECS::TExclude<FChangeTrackedTag>{}).ForEach([&](const FChangeTrackedValue& Value) { Sum += Value.Value; });
    EXPECT_EQ(Sum, 4) << "a missing exclude excludes nothing";

    EXPECT_FALSE(Registry.FindStorage<FChangeTrackedNeverCreated>().IsValid());
    EXPECT_FALSE(Registry.FindStorage<FChangeTrackedTag>().IsValid());
}

TEST(ECSChangeTracking, AssureStoragesCreatesEveryListedPool)
{
    ECS::FRegistry Registry;
    Registry.AssureStorages<FChangeTrackedValue, const FChangeTrackedOther, FChangeTrackedTag>();
    EXPECT_TRUE(Registry.FindStorage<FChangeTrackedValue>().IsValid());
    EXPECT_TRUE(Registry.FindStorage<FChangeTrackedOther>().IsValid());
    EXPECT_TRUE(Registry.FindStorage<FChangeTrackedTag>().IsValid());

    ECS::FRegistry ById;
    const ECS::FComponentTypeID TypeIDs[] = { ECS::GetComponentTypeID<FChangeTrackedValue>(), ECS::GetComponentTypeID<FChangeTrackedOther>() };
    ById.AssureStorages(TypeIDs);
    EXPECT_TRUE(ById.FindStorage<FChangeTrackedValue>().IsValid());
    EXPECT_TRUE(ById.FindStorage<FChangeTrackedOther>().IsValid());
}

TEST(ECSChangeTracking, ChangeTicksReportEveryMutableAccessAndNoRead)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    const ECS::FEntity C = Registry.Create();
    const ECS::FEntity D = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedValue>(B);
    Registry.Emplace<FChangeTrackedValue>(D);
    EXPECT_EQ(ChangedSince(Registry, 0).Num(), 3) << "a first read with no history sees every element";

    const uint32 Since = Registry.AdvanceChangeTick();
    EXPECT_TRUE(ChangedSince(Registry, Since).IsEmpty());

    const ECS::FRegistry& ReadOnly = Registry;
    int32 Sum = ReadOnly.Get<FChangeTrackedValue>(A).Value + Registry.Get<ECS::Read<FChangeTrackedValue>>(B).Value + Registry.TryGet<ECS::Read<FChangeTrackedValue>>(D)->Value;
    Sum += ReadOnly.TryGet<FChangeTrackedValue>(A)->Value;
    EXPECT_EQ(Sum, 0);
    EXPECT_TRUE(ChangedSince(Registry, Since).IsEmpty()) << "const access never stamps";

    const uint64 EpochBefore = Registry.GetWriteEpoch<FChangeTrackedValue>();
    Registry.Get<FChangeTrackedValue>(A).Value = 2;
    Registry.Patch<FChangeTrackedValue>(B, [](FChangeTrackedValue& Value) { Value.Value = 1; });
    Registry.Emplace<FChangeTrackedValue>(C);
    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), EpochBefore);

    const TVector<ECS::FEntity> Changed = ChangedSince(Registry, Since);
    EXPECT_EQ(Changed.Num(), 3);
    EXPECT_TRUE(Algo::Contains(Changed, A)) << "a mutable Get counts as a write";
    EXPECT_TRUE(Algo::Contains(Changed, B));
    EXPECT_TRUE(Algo::Contains(Changed, C));
    EXPECT_FALSE(HasChangedSince<FChangeTrackedValue>(Registry, D, Since));
}

TEST(ECSChangeTracking, MutableGetMovesTheEpochOncePerTick)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);

    (void)Registry.AdvanceChangeTick();
    (void)Registry.TryGet<FChangeTrackedValue>(A);
    const uint64 AfterFirst = Registry.GetWriteEpoch<FChangeTrackedValue>();
    (void)Registry.TryGet<FChangeTrackedValue>(A);
    (void)Registry.Get<FChangeTrackedValue>(A);
    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedValue>(), AfterFirst) << "stamps of one tick move the epoch once";

    (void)Registry.AdvanceChangeTick();
    (void)Registry.Get<FChangeTrackedValue>(A);
    EXPECT_NE(Registry.GetWriteEpoch<FChangeTrackedValue>(), AfterFirst);
}

TEST(ECSChangeTracking, RawWritePathsStampWithoutSignals)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();
    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedValue>(B);

    int32 Updates = 0;
    Registry.GetSignals<FChangeTrackedValue>().OnUpdate.Connect<&CountSignal>(&Updates);

    const uint32 Since = Registry.AdvanceChangeTick();
    Registry.MarkChangedRaw(ECS::GetComponentTypeID<FChangeTrackedValue>(), A);
    EXPECT_EQ(Updates, 0) << "MarkChanged records the write and stays silent";

    ECS::FSparseSet* Set = Registry.FindStorage<FChangeTrackedValue>().GetSet();
    EXPECT_NE(Set->GetRawForWrite(B), nullptr);

    const TVector<ECS::FEntity> Changed = ChangedSince(Registry, Since);
    EXPECT_EQ(Changed.Num(), 2);
}

TEST(ECSChangeTracking, AdvanceExpiresOnlyEscapedPointersOfTrackedPools)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedOther>(A);

    ECS::FSparseSet* Tracked = Registry.FindStorage<FChangeTrackedValue>().GetSet();
    ECS::FSparseSet* Untracked = Registry.FindStorage<FChangeTrackedOther>().GetSet();
    Untracked->MarkPointersEscaped();

    const uint32 TrackedLayout = *Tracked->GetLayoutEpoch();
    (void)Registry.AdvanceChangeTick();
    EXPECT_EQ(*Tracked->GetLayoutEpoch(), TrackedLayout) << "nothing escaped yet, so no cached reference has to look again";

    Tracked->MarkPointersEscaped();
    const uint32 UntrackedLayout = *Untracked->GetLayoutEpoch();
    (void)Registry.AdvanceChangeTick();
    EXPECT_NE(*Tracked->GetLayoutEpoch(), TrackedLayout);
    EXPECT_EQ(*Untracked->GetLayoutEpoch(), UntrackedLayout);
}

TEST(ECSChangeTracking, ChangeTicksMoveWithTheSwapRemovedEntry)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    const ECS::FEntity C = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedValue>(B);
    Registry.Emplace<FChangeTrackedValue>(C);

    const uint32 Since = Registry.AdvanceChangeTick();
    Registry.Patch<FChangeTrackedValue>(C, [](FChangeTrackedValue& Value) { Value.Value = 5; });

    const ECS::FSparseSet* Set = Registry.FindStorage<FChangeTrackedValue>().GetSet();
    const uint32 LastSlot = Set->GetDenseIndex(C);
    Registry.Remove<FChangeTrackedValue>(A);
    ASSERT_NE(Set->GetDenseIndex(C), LastSlot) << "removing the first entry swaps the last one into its slot";

    EXPECT_TRUE(HasChangedSince<FChangeTrackedValue>(Registry, C, Since));
    EXPECT_FALSE(HasChangedSince<FChangeTrackedValue>(Registry, B, Since));

    const TVector<ECS::FEntity> Changed = ChangedSince(Registry, Since);
    ASSERT_EQ(Changed.Num(), 1);
    EXPECT_EQ(Changed[0], C);
}

TEST(ECSChangeTracking, ChangeTicksSurviveCompactionOfAStablePool)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedStable>();

    TVector<ECS::FEntity> Entities;
    for (int32 Index = 0; Index < 6; ++Index)
    {
        Entities.Add(Registry.Create());
        Registry.Emplace<FChangeTrackedStable>(Entities.Last());
    }

    const uint32 Since = Registry.AdvanceChangeTick();
    Registry.Patch<FChangeTrackedStable>(Entities[5], [](FChangeTrackedStable& Value) { Value.Value = 1; });
    Registry.Remove<FChangeTrackedStable>(Entities[0]);
    Registry.Remove<FChangeTrackedStable>(Entities[2]);
    Registry.Compact();

    for (int32 Index : { 1, 3, 4 })
    {
        EXPECT_FALSE(HasChangedSince<FChangeTrackedStable>(Registry, Entities[Index], Since));
    }
    EXPECT_TRUE(HasChangedSince<FChangeTrackedStable>(Registry, Entities[5], Since));
}

TEST(ECSChangeTracking, UntrackedPoolReportsEveryEntity)
{
    ECS::FRegistry Registry;
    Registry.Emplace<FChangeTrackedOther>(Registry.Create());
    Registry.Emplace<FChangeTrackedOther>(Registry.Create());

    const uint32 Since = Registry.AdvanceChangeTick();
    EXPECT_EQ(ChangedSinceOf<FChangeTrackedOther>(Registry, Since).Num(), 2);
}

TEST(ECSChangeTracking, MutableViewsStampAndConstViewsDoNot)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedValue>(B);
    Registry.Emplace<FChangeTrackedOther>(B);

    const uint32 Since = Registry.AdvanceChangeTick();

    int32 Sum = 0;
    Registry.View<const FChangeTrackedValue>().ForEach([&](const FChangeTrackedValue& Value) { Sum += Value.Value; });
    Registry.FindView<const FChangeTrackedValue>(ECS::TExclude<FChangeTrackedOther>{}).ForEach([&](const FChangeTrackedValue& Value) { Sum += Value.Value; });
    EXPECT_FALSE(Registry.View<const FChangeTrackedValue>().StampsChanges());
    EXPECT_FALSE(Registry.View<FChangeTrackedOther>().StampsChanges()) << "an untracked include never stamps";
    EXPECT_TRUE(ChangedSince(Registry, Since).IsEmpty()) << "a const include stamps nothing";

    Registry.View<FChangeTrackedValue, const FChangeTrackedOther>().ForEach([](FChangeTrackedValue&, const FChangeTrackedOther&) {});
    const TVector<ECS::FEntity> Filtered = ChangedSince(Registry, Since);
    ASSERT_EQ(Filtered.Num(), 1);
    EXPECT_EQ(Filtered[0], B) << "a filtered walk stamps only the entities it visits";

    Registry.View<FChangeTrackedValue>().ForEach([](FChangeTrackedValue& Value) { ++Value.Value; });
    EXPECT_EQ(ChangedSince(Registry, Since).Num(), 2);
}

TEST(ECSChangeTracking, MutableViewsStampThroughEachAndGet)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);
    Registry.Emplace<FChangeTrackedValue>(B);
    Registry.Emplace<FChangeTrackedOther>(A);

    uint32 Since = Registry.AdvanceChangeTick();
    {
        auto View = Registry.View<FChangeTrackedValue>();
        (void)View.Get<const FChangeTrackedValue>(B);
        EXPECT_TRUE(ChangedSince(Registry, Since).IsEmpty()) << "a const read through a mutable include stamps nothing";
        View.Get<FChangeTrackedValue>(B).Value = 1;
    }
    TVector<ECS::FEntity> Changed = ChangedSince(Registry, Since);
    ASSERT_EQ(Changed.Num(), 1);
    EXPECT_EQ(Changed[0], B);

    Since = Registry.AdvanceChangeTick();
    for (auto [Entity, Value, Other] : Registry.View<FChangeTrackedValue, const FChangeTrackedOther>().Each())
    {
        Value.Value = Other.Value;
    }
    Changed = ChangedSince(Registry, Since);
    ASSERT_EQ(Changed.Num(), 1);
    EXPECT_EQ(Changed[0], A);
}

TEST(ECSChangeTracking, ScheduledRunsLeaveTrackedPoolsAlone)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FChangeTrackedValue>();
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<FChangeTrackedValue>(A);

    const uint64 Epoch = Registry.GetWriteEpoch<FChangeTrackedValue>();

    FSystemAccess Writer;
    Writer.Write<FChangeTrackedValue>();
    MarkDeclaredWrites(Registry, Writer);
    MarkDeclaredWrites(Registry, FSystemAccess::Exclusive());

    EXPECT_EQ(Registry.GetWriteEpoch<FChangeTrackedValue>(), Epoch) << "a tracked pool reports its own writes, so a scheduled run says nothing";
}

TEST(ECSChangeTracking, RuntimeViewDrivesByDenseSlots)
{
    ECS::FRegistry Registry;
    TVector<ECS::FEntity> Entities;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Entities.Add(Registry.Create());
        Registry.Emplace<FChangeTrackedStable>(Entities.Last());
    }
    for (int32 Index = 0; Index < 6; ++Index)
    {
        Registry.Remove<FChangeTrackedStable>(Entities[Index]);
    }
    for (int32 Index = 0; Index < 4; ++Index)
    {
        Registry.Emplace<FChangeTrackedOther>(Entities[4 + (Index % 4)]);
    }

    const ECS::FSparseSet& Stable = *Registry.FindStorage<FChangeTrackedStable>().GetSet();
    const ECS::FSparseSet& Packed = *Registry.FindStorage<FChangeTrackedOther>().GetSet();
    ASSERT_LT(Stable.Num(), Packed.Num());
    ASSERT_GT(Stable.GetDenseSize(), Packed.GetDenseSize());

    ECS::FRuntimeView View;
    View.AddInclude(Stable);
    View.AddInclude(Packed);
    EXPECT_TRUE(View.IsDrivenBy(&Packed)) << "the fewer live entries still walk more tombstoned slots";

    int32 Visited = 0;
    View.ForEach([&](ECS::FEntity) { ++Visited; });
    EXPECT_EQ(Visited, 2);
}
