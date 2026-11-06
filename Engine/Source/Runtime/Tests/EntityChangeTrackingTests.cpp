#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/ECS/Query.h"
#include "World/World.h"
#include "World/Entity/Systems/CameraRigSystem.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace EntityChangeTrackingTestsInternal
{
    struct FTrackedValue
    {
        int32 Value = 0;

        bool operator==(const FTrackedValue&) const = default;
    };

    struct FTrackedTag {};

    constexpr int32 RowCount = 8;

    TVector<ECS::FEntity> CreateRows(ECS::FRegistry& Registry)
    {
        TVector<ECS::FEntity> Rows;
        for (int32 Index = 0; Index < RowCount; ++Index)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<FTrackedValue>(Entity).Value = Index;
            Registry.Emplace<FTrackedTag>(Entity);
            Rows.Add(Entity);
        }
        Registry.TrackChanges<FTrackedValue>();
        return Rows;
    }

    template<typename T = FTrackedValue>
    bool HasChangedSince(const ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Since)
    {
        return Registry.FindStorage(ECS::GetComponentTypeID<T>())->HasChangedSince(Entity, Since);
    }

    ECS::FEntity CreatePlaced(ECS::FRegistry& Registry)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity).Bind(Registry, Entity);
        return Entity;
    }

    // The rows whose index makes Expected true are exactly the ones reported changed since Since.
    template<typename TExpected>
    void ExpectChangedRows(const ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Rows, uint32 Since, TExpected Expected)
    {
        for (int32 Index = 0; Index < RowCount; ++Index)
        {
            EXPECT_EQ(HasChangedSince(Registry, Rows[Index], Since), Expected(Index)) << "row " << Index;
        }
    }
}

using namespace EntityChangeTrackingTestsInternal;

TEST(EntityChangeTracking, VisitedButUnwrittenRowIsNotChanged)
{
    ECS::FRegistry Registry;
    const TVector<ECS::FEntity> Rows = CreateRows(Registry);
    const uint32 Since = Registry.AdvanceChangeTick();

    ECS::TQuery<ECS::Write<FTrackedValue>> Query;
    int32 Sum = 0;
    int32 Visited = 0;
    Query.ForEach(Registry, [&](ECS::TMut<FTrackedValue> Row)
    {
        Sum += Row.Get().Value;
        ++Visited;
    });
    Query.ForEach(Registry, [&](const FTrackedValue& Row) { Sum += Row.Value; });
    for (auto [Entity, Row] : Registry.View<ECS::Write<FTrackedValue>>().Each())
    {
        Sum += (*std::as_const(Row)).Value;
    }

    EXPECT_EQ(Visited, RowCount);
    EXPECT_GT(Sum, 0);
    ExpectChangedRows(Registry, Rows, Since, [](int32) { return false; });
}

TEST(EntityChangeTracking, MutatedRowIsChanged)
{
    ECS::FRegistry Registry;
    const TVector<ECS::FEntity> Rows = CreateRows(Registry);
    const uint32 Since = Registry.AdvanceChangeTick();

    ECS::TQuery<ECS::Write<FTrackedValue>, ECS::With<FTrackedTag>> Query;
    Query.ForEach(Registry, [&](ECS::TMut<FTrackedValue> Row)
    {
        if (Row.Get().Value % 3 == 0)
        {
            Row->Value += 100;
        }
    });

    ExpectChangedRows(Registry, Rows, Since, [](int32 Index) { return Index % 3 == 0; });
}

TEST(EntityChangeTracking, SetIfChangedStampsOnlyARealChange)
{
    ECS::FRegistry Registry;
    const TVector<ECS::FEntity> Rows = CreateRows(Registry);
    const uint32 Since = Registry.AdvanceChangeTick();

    ECS::TQuery<ECS::Write<FTrackedValue>> Query;
    Query.ForEach(Registry, [&](ECS::FEntity Entity, ECS::TMut<FTrackedValue> Row)
    {
        const bool bWrite = Entity == Rows[5];
        EXPECT_EQ(Row.SetIfChanged(FTrackedValue{ bWrite ? -5 : Row.Get().Value }), bWrite);
        EXPECT_FALSE(Row.SetIfChanged(&FTrackedValue::Value, Row.Get().Value));
    });

    EXPECT_EQ(Registry.Get<ECS::Read<FTrackedValue>>(Rows[5]).Value, -5);
    ExpectChangedRows(Registry, Rows, Since, [](int32 Index) { return Index == 5; });
}

TEST(EntityChangeTracking, BypassChangeDetectionNeverStamps)
{
    ECS::FRegistry Registry;
    const TVector<ECS::FEntity> Rows = CreateRows(Registry);
    const uint32 Since = Registry.AdvanceChangeTick();

    ECS::TQuery<ECS::Write<FTrackedValue>> Query;
    Query.ParallelForEach(Registry, [](ECS::TMut<FTrackedValue> Row) { Row.BypassChangeDetection().Value += 1; }, 1);

    EXPECT_EQ(Registry.Get<ECS::Read<FTrackedValue>>(Rows[0]).Value, 1);
    ExpectChangedRows(Registry, Rows, Since, [](int32) { return false; });
}

TEST(EntityChangeTracking, ChangedFilterSeesExactlyTheWrittenRows)
{
    ECS::FRegistry Registry;
    const TVector<ECS::FEntity> Rows = CreateRows(Registry);

    ECS::TQuery<ECS::Read<FTrackedValue>, ECS::Changed<FTrackedValue>> Reader;
    Reader.ForEach(Registry, [](const FTrackedValue&) {});

    ECS::TQuery<ECS::Write<FTrackedValue>, ECS::Read<FTrackedTag>> Writer;
    Writer.ForEach(Registry, [&](ECS::FEntity Entity, ECS::TMut<FTrackedValue> Row)
    {
        if (Entity == Rows[2] || Entity == Rows[6])
        {
            (*Row).Value = -1;
        }
    });

    TVector<ECS::FEntity> Seen;
    Reader.ForEach(Registry, [&](ECS::FEntity Entity, const FTrackedValue&) { Seen.Add(Entity); });
    ASSERT_EQ(Seen.Num(), 2);
    EXPECT_TRUE(Seen.Contains(Rows[2]));
    EXPECT_TRUE(Seen.Contains(Rows[6]));
}

TEST(EntityChangeTracking, SettledCameraRigLeavesItsTransformUnchanged)
{
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, FName("CameraRigChangeTestWorld"), FGuid::New(), OF_Transient);
    TStrongObjectPtr<CWorld> WorldOwner = World;
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    Registry.Ctx().Emplace<CWorld*>(World);
    Registry.TrackChanges<STransformComponent>();

    const ECS::FEntity Target = CreatePlaced(Registry);
    const ECS::FEntity Follower = CreatePlaced(Registry);
    Registry.Emplace<SCameraFollowComponent>(Follower).Target = Target;
    const ECS::FEntity Arm = CreatePlaced(Registry);
    Registry.Emplace<SSpringArmComponent>(Arm).Target = Target;

    CCameraRigSystem* System = NewObject<CCameraRigSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    System->SetOwningWorld(World);
    System->ConfigureSystem();
    System->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));

    auto Step = [&]
    {
        const uint32 Since = Registry.AdvanceChangeTick();
        SSystemContext Context = FWorldTestAccess::SystemContext(*World);
        Context.DeltaTime = 0.1;
        System->OnUpdate(Context);
        return Since;
    };
    auto ExpectRigsChanged = [&](uint32 Since, bool bExpected)
    {
        EXPECT_EQ(HasChangedSince<STransformComponent>(Registry, Follower, Since), bExpected) << "follower";
        EXPECT_EQ(HasChangedSince<STransformComponent>(Registry, Arm, Since), bExpected) << "spring arm";
    };

    ExpectRigsChanged(Step(), true);
    ExpectRigsChanged(Step(), false);

    Registry.Get<STransformComponent>(Target).SetLocation(FVector3(5.0f, 0.0f, 0.0f));
    ExpectRigsChanged(Step(), true);
    EXPECT_GT(Registry.Get<STransformComponent>(Follower).GetWorldLocation().x, 0.0f) << "the follower lags toward the moved target";
    EXPECT_NEAR(Registry.Get<STransformComponent>(Arm).GetWorldLocation().x, 5.0f, 1e-4f) << "the arm pivot has no lag by default";

    System->OnTeardown(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
    System->SetOwningWorld(nullptr);
}
