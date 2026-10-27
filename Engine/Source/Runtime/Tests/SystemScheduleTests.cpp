#include <gtest/gtest.h>

#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#include "Scripting/ScriptableTest.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    // Only stored and handed back by the driver, so a sentinel stands in for a real world.
    CWorld* const ScheduleSentinelWorld = reinterpret_cast<CWorld*>(0x5151);

    struct FScheduleSystems
    {
        FScheduleSystems()
        {
            RegisterPendingTypes();
            CEntitySystemTest::bAllowCreation = true;

            const CClass* Wanted[] = {
                CEntitySystemTest::StaticClass(), CEntitySystemExclusiveTest::StaticClass(), CEntitySystemReaderTest::StaticClass(),
                CEntitySystemAfterTest::StaticClass(), CEntitySystemLastTest::StaticClass(), CEntitySystemCycleTest::StaticClass() };

            THashSet<FName> Disabled;
            EntitySystems::ForEachSystemClass([&](CClass* Class)
            {
                if (!Algo::Contains(Wanted, Class))
                {
                    Disabled.insert(Class->GetName());
                }
            });
            EntitySystems::CreateMissing(*ScheduleSentinelWorld, Disabled, Systems);

            Writer    = EntitySystems::Find(Systems, CEntitySystemTest::StaticClass());
            Exclusive = EntitySystems::Find(Systems, CEntitySystemExclusiveTest::StaticClass());
            Reader    = EntitySystems::Find(Systems, CEntitySystemReaderTest::StaticClass());
            After     = EntitySystems::Find(Systems, CEntitySystemAfterTest::StaticClass());
            Last      = EntitySystems::Find(Systems, CEntitySystemLastTest::StaticClass());
            Cycle     = EntitySystems::Find(Systems, CEntitySystemCycleTest::StaticClass());
        }

        ~FScheduleSystems()
        {
            EntitySystems::DestroyAll(SSystemContext(), Systems);
            CEntitySystemTest::bAllowCreation = false;
        }

        bool AllCreated() const
        {
            return Writer != nullptr && Exclusive != nullptr && Reader != nullptr && After != nullptr && Last != nullptr && Cycle != nullptr;
        }

        TVector<TStrongObjectPtr<CEntitySystem>> Systems;
        CEntitySystem* Writer    = nullptr;
        CEntitySystem* Exclusive = nullptr;
        CEntitySystem* Reader    = nullptr;
        CEntitySystem* After     = nullptr;
        CEntitySystem* Last      = nullptr;
        CEntitySystem* Cycle     = nullptr;
    };

    TVector<uint8> AllWork(const CWorld::FStageSchedule& Stage)
    {
        return TVector<uint8>(Stage.Systems.size(), 1);
    }

    size_t IndexOf(const CWorld::FStageSchedule& Stage, const CEntitySystem* System)
    {
        return (size_t)(Algo::Find(Stage.Systems, System) - Stage.Systems.begin());
    }
}

TEST(SystemSchedule, ADeclaredDependencyOrdersSystemsWhateverTheirNames)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // Named first alphabetically, so only the declared order can put the reader ahead of it.
    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Last, S.After, S.Reader }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.size(), 3u);
    EXPECT_EQ(Stage.Systems[0], S.Reader);
    EXPECT_EQ(Stage.Systems[1], S.After);
    EXPECT_EQ(Stage.Systems[2], S.Last) << "a RunsBefore declared by name orders the system it names";
}

TEST(SystemSchedule, ADependencySeparatesSystemsThatNeverConflict)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.After, S.Reader }, EUpdateStage::PrePhysics);
    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(Stage, AllWork(Stage));
    ASSERT_EQ(Batches.size(), 2u) << "two readers share a batch unless one declared it runs after the other";
}

TEST(SystemSchedule, AnIdleSystemStillKeepsTheSystemsAroundItInOrder)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Reader, S.After, S.Last }, EUpdateStage::PrePhysics);
    TVector<uint8> WithWork = AllWork(Stage);
    WithWork[IndexOf(Stage, S.After)] = 0;

    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(Stage, WithWork);
    ASSERT_EQ(Batches.size(), 2u) << "the reader to last order runs through the idle system between them";
    EXPECT_EQ(Batches[0].Members, (TVector<uint16>{ (uint16)IndexOf(Stage, S.Reader) }));
    EXPECT_EQ(Batches[1].Members, (TVector<uint16>{ (uint16)IndexOf(Stage, S.Last) }));
}

TEST(SystemSchedule, UnorderedExclusiveSystemsGatherAfterTheSharedOnes)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule First  = SystemSchedule::OrderStage({ S.Exclusive, S.Writer, S.Reader }, EUpdateStage::PrePhysics);
    const CWorld::FStageSchedule Second = SystemSchedule::OrderStage({ S.Reader, S.Exclusive, S.Writer }, EUpdateStage::PrePhysics);
    ASSERT_EQ(First.Systems, Second.Systems) << "the order must not depend on discovery order";
    EXPECT_EQ(First.Systems.back(), S.Exclusive);

    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(First, AllWork(First));
    ASSERT_EQ(Batches.size(), 2u);
    EXPECT_EQ(Batches[0].Members.size(), 2u) << "the shared systems run together ahead of the exclusive one";
}

TEST(SystemSchedule, AnIdleExclusiveSystemStopsSeparatingTheSystemsAroundIt)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // The exclusive system is put between the other two by hand, the one place it can split them.
    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Exclusive, S.Reader };
    Stage.Precedes = TVector<uint8>(9, 0);

    ASSERT_EQ(SystemSchedule::BuildBatches(Stage, AllWork(Stage)).size(), 3u) << "an exclusive system with work is a barrier";

    TVector<uint8> WithWork = AllWork(Stage);
    WithWork[1] = 0;
    const TVector<CWorld::FSystemBatch> Idle = SystemSchedule::BuildBatches(Stage, WithWork);
    ASSERT_EQ(Idle.size(), 1u) << "without the barrier the writer and the reader never conflict";
    EXPECT_EQ(Idle[0].Members.size(), 2u);
}

TEST(SystemSchedule, AConflictWaitsForTheEarlierSystem)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // The writer conflicts with itself, so a second copy lands in a later batch even with nothing ordered between them.
    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Reader, S.Writer };
    Stage.Precedes = TVector<uint8>(9, 0);

    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(Stage, AllWork(Stage));
    ASSERT_EQ(Batches.size(), 2u);
    EXPECT_EQ(Batches[0].Members, (TVector<uint16>{ 0, 1 }));
    EXPECT_EQ(Batches[1].Members, (TVector<uint16>{ 2 }));
}

TEST(SystemSchedule, ADependencyCycleStillSchedulesEverySystem)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Cycle, S.Last, S.After, S.Reader }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.size(), 4u);
    for (const CEntitySystem* System : { S.Reader, S.After, S.Last, S.Cycle })
    {
        EXPECT_NE(IndexOf(Stage, System), Stage.Systems.size());
    }

    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(Stage, AllWork(Stage));
    size_t Scheduled = 0;
    for (const CWorld::FSystemBatch& Batch : Batches)
    {
        Scheduled += Batch.Members.size();
    }
    EXPECT_EQ(Scheduled, 4u) << "breaking the cycle must not drop a system from the stage";
}

TEST(SystemSchedule, DependenciesOnlyApplyBetweenSystemsInTheSameStage)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.After }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.size(), 1u) << "a dependency on a system absent from the stage is ignored";
    EXPECT_TRUE(SystemSchedule::DeclaresOrder(S.Reader, S.After));
    EXPECT_FALSE(SystemSchedule::DeclaresOrder(S.After, S.Reader));
}
