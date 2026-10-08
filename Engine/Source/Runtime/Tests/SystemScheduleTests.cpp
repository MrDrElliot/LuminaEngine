#include <gtest/gtest.h>

#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#include "Scripting/ScriptableTest.h"
#include "World/Entity/Systems/EntitySystem.h"
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
            ProcessNewlyLoadedCObjects();
            CEntitySystemTest::bAllowCreation = true;

            THashSet<FName> Disabled;
            EntitySystems::ForEachSystemClass([&Disabled](CClass* Class)
            {
                if (Class != CEntitySystemTest::StaticClass()
                    && Class != CEntitySystemExclusiveTest::StaticClass()
                    && Class != CEntitySystemReaderTest::StaticClass())
                {
                    Disabled.insert(Class->GetName());
                }
            });
            EntitySystems::CreateMissing(*ScheduleSentinelWorld, Disabled, Systems);

            Writer    = EntitySystems::Find(Systems, CEntitySystemTest::StaticClass());
            Exclusive = Cast<CEntitySystemExclusiveTest>(EntitySystems::Find(Systems, CEntitySystemExclusiveTest::StaticClass()));
            Reader    = EntitySystems::Find(Systems, CEntitySystemReaderTest::StaticClass());
        }

        ~FScheduleSystems()
        {
            EntitySystems::DestroyAll(Systems);
            CEntitySystemTest::bAllowCreation = false;
        }

        TVector<TStrongObjectPtr<CEntitySystem>> Systems;
        CEntitySystem*              Writer    = nullptr;
        CEntitySystemExclusiveTest* Exclusive = nullptr;
        CEntitySystem*              Reader    = nullptr;
    };

    TVector<uint8> AllWork(const TVector<CWorld::FStageSlot>& Stage)
    {
        return TVector<uint8>(Stage.size(), 1);
    }
}

TEST(SystemSchedule, AnIdleExclusiveSystemStopsSeparatingTheSystemsAroundIt)
{
    FScheduleSystems S;
    ASSERT_NE(S.Writer, nullptr);
    ASSERT_NE(S.Exclusive, nullptr);
    ASSERT_NE(S.Reader, nullptr);

    TVector<CWorld::FStageSlot> Stage = { { S.Writer, 0 }, { S.Exclusive, 64 }, { S.Reader, 128 } };

    const TVector<CWorld::FSystemBatch> Busy = SystemSchedule::BuildBatches(Stage, AllWork(Stage));
    ASSERT_EQ(Busy.size(), 3u) << "an exclusive system with work is a barrier between its neighbors";

    TVector<uint8> WithWork = AllWork(Stage);
    WithWork[1] = 0;
    const TVector<CWorld::FSystemBatch> Idle = SystemSchedule::BuildBatches(Stage, WithWork);
    ASSERT_EQ(Idle.size(), 1u) << "without the barrier the writer and the reader never conflict";
    EXPECT_EQ(Idle[0].Members.size(), 2u);
}

TEST(SystemSchedule, EqualPriorityPutsExclusiveSystemsLastWhateverTheInputOrder)
{
    FScheduleSystems S;
    ASSERT_NE(S.Exclusive, nullptr);

    TVector<CWorld::FStageSlot> First  = { { S.Exclusive, 32 }, { S.Writer, 32 }, { S.Reader, 32 } };
    TVector<CWorld::FStageSlot> Second = { { S.Reader, 32 }, { S.Exclusive, 32 }, { S.Writer, 32 } };
    SystemSchedule::SortStage(First);
    SystemSchedule::SortStage(Second);

    for (size_t i = 0; i < First.size(); ++i)
    {
        EXPECT_EQ(First[i].System, Second[i].System) << "the order must not depend on discovery order";
    }
    EXPECT_EQ(First.back().System, S.Exclusive);

    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(First, AllWork(First));
    ASSERT_EQ(Batches.size(), 2u);
    EXPECT_EQ(Batches[0].Members.size(), 2u) << "the declared systems at that priority share the first batch";
}

TEST(SystemSchedule, AHigherPriorityConflictStillRunsFirst)
{
    FScheduleSystems S;

    // The writer conflicts with itself, so a second slot of it must land in a later batch even though nothing else is between them.
    TVector<CWorld::FStageSlot> Stage = { { S.Writer, 0 }, { S.Reader, 32 }, { S.Writer, 64 } };
    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildBatches(Stage, AllWork(Stage));
    ASSERT_EQ(Batches.size(), 2u);
    EXPECT_EQ(Batches[0].Members, (TVector<uint16>{ 0, 1 }));
    EXPECT_EQ(Batches[1].Members, (TVector<uint16>{ 2 }));
}
