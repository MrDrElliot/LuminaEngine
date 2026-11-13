#include <gtest/gtest.h>

#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Log/Log.h"

#include "Scripting/ScriptableTest.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/World.h"
#include "WorldTestAccess.h"

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

            const THashSet<FName> Disabled = FWorldTestAccess::AllSystemsExcept({
                CEntitySystemTest::StaticClass()->GetName(), CEntitySystemExclusiveTest::StaticClass()->GetName(),
                CEntitySystemReaderTest::StaticClass()->GetName(), CEntitySystemAfterTest::StaticClass()->GetName(),
                CEntitySystemLastTest::StaticClass()->GetName(), CEntitySystemCycleTest::StaticClass()->GetName(),
                CEntitySystemTransformReaderTest::StaticClass()->GetName() });
            EntitySystems::CreateMissing(*ScheduleSentinelWorld, Disabled, Systems);

            Writer    = EntitySystems::Find(Systems, CEntitySystemTest::StaticClass());
            Exclusive = EntitySystems::Find(Systems, CEntitySystemExclusiveTest::StaticClass());
            Reader    = EntitySystems::Find(Systems, CEntitySystemReaderTest::StaticClass());
            After     = EntitySystems::Find(Systems, CEntitySystemAfterTest::StaticClass());
            Last      = EntitySystems::Find(Systems, CEntitySystemLastTest::StaticClass());
            Cycle     = EntitySystems::Find(Systems, CEntitySystemCycleTest::StaticClass());
            TransformReader = EntitySystems::Find(Systems, CEntitySystemTransformReaderTest::StaticClass());
        }

        ~FScheduleSystems()
        {
            EntitySystems::DestroyAll(SSystemContext(), Systems);
            CEntitySystemTest::bAllowCreation = false;
        }

        bool AllCreated() const
        {
            return Writer != nullptr && Exclusive != nullptr && Reader != nullptr && After != nullptr && Last != nullptr && Cycle != nullptr
                && TransformReader != nullptr;
        }

        TVector<TStrongObjectPtr<CEntitySystem>> Systems;
        CEntitySystem* Writer    = nullptr;
        CEntitySystem* Exclusive = nullptr;
        CEntitySystem* Reader    = nullptr;
        CEntitySystem* After     = nullptr;
        CEntitySystem* Last      = nullptr;
        CEntitySystem* Cycle     = nullptr;
        CEntitySystem* TransformReader = nullptr;
    };

    TVector<uint8> AllWork(const CWorld::FStageSchedule& Stage)
    {
        return TVector<uint8>(Stage.Systems.Num(), 1);
    }

    TVector<CWorld::FSystemBatch> Levels(const CWorld::FStageSchedule& Stage, const TVector<uint8>& WithWork)
    {
        return SystemSchedule::BuildLevels(SystemSchedule::BuildSegments(Stage, WithWork));
    }

    int32 SegmentOf(const TVector<CWorld::FSystemSegment>& Segments, int32 StageIndex)
    {
        for (int32 Index = 0; Index < Segments.Num(); ++Index)
        {
            if (Algo::Contains(Segments[Index].Members, (uint16)StageIndex))
            {
                return Index;
            }
        }
        return -1;
    }

    int32 IndexOf(const CWorld::FStageSchedule& Stage, const CEntitySystem* System)
    {
        return Stage.Systems.Find(System);
    }
}

TEST(SystemSchedule, ADeclaredDependencyOrdersSystemsWhateverTheirNames)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // Named first alphabetically, so only the declared order can put the reader ahead of it.
    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Last, S.After, S.Reader }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.Num(), 3);
    EXPECT_EQ(Stage.Systems[0], S.Reader);
    EXPECT_EQ(Stage.Systems[1], S.After);
    EXPECT_EQ(Stage.Systems[2], S.Last) << "a RunsBefore declared by name orders the system it names";
}

TEST(SystemSchedule, ADependencySeparatesSystemsThatNeverConflict)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.After, S.Reader }, EUpdateStage::PrePhysics);
    const TVector<CWorld::FSystemBatch> Batches = Levels(Stage, AllWork(Stage));
    ASSERT_EQ(Batches.Num(), 2) << "two readers share a batch unless one declared it runs after the other";
}

TEST(SystemSchedule, AnIdleSystemStillKeepsTheSystemsAroundItInOrder)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Reader, S.After, S.Last }, EUpdateStage::PrePhysics);
    TVector<uint8> WithWork = AllWork(Stage);
    WithWork[IndexOf(Stage, S.After)] = 0;

    const TVector<CWorld::FSystemBatch> Batches = Levels(Stage, WithWork);
    ASSERT_EQ(Batches.Num(), 2) << "the reader to last order runs through the idle system between them";
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
    EXPECT_EQ(First.Systems.Last(), S.Exclusive);

    const TVector<CWorld::FSystemBatch> Batches = Levels(First, AllWork(First));
    ASSERT_EQ(Batches.Num(), 2);
    EXPECT_EQ(Batches[0].Members.Num(), 2) << "the shared systems run together ahead of the exclusive one";
}

TEST(SystemSchedule, AnIdleExclusiveSystemStopsSeparatingTheSystemsAroundIt)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // The exclusive system is put between the other two by hand, the one place it can split them.
    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Exclusive, S.Reader };
    Stage.Precedes = TVector<uint8>(9, 0);

    ASSERT_EQ(Levels(Stage, AllWork(Stage)).Num(), 3) << "an exclusive system with work is a barrier";

    TVector<uint8> WithWork = AllWork(Stage);
    WithWork[1] = 0;
    const TVector<CWorld::FSystemBatch> Idle = Levels(Stage, WithWork);
    ASSERT_EQ(Idle.Num(), 1) << "without the barrier the writer and the reader never conflict";
    EXPECT_EQ(Idle[0].Members.Num(), 2);
}

TEST(SystemSchedule, AConflictWaitsForTheEarlierSystem)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // The writer conflicts with itself, so a second copy lands in a later batch even with nothing ordered between them.
    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Reader, S.Writer };
    Stage.Precedes = TVector<uint8>(9, 0);

    const TVector<CWorld::FSystemBatch> Batches = Levels(Stage, AllWork(Stage));
    ASSERT_EQ(Batches.Num(), 2);
    EXPECT_EQ(Batches[0].Members, (TVector<uint16>{ 0, 1 }));
    EXPECT_EQ(Batches[1].Members, (TVector<uint16>{ 2 }));
}

TEST(SystemSchedule, DisjointSystemsShareABatchAndConflictingOnesDoNot)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.TransformReader, S.Reader, S.Writer }, EUpdateStage::PrePhysics);
    const TVector<CWorld::FSystemSegment> Segments = SystemSchedule::BuildSegments(Stage, AllWork(Stage));
    const TVector<CWorld::FSystemBatch> Batches = SystemSchedule::BuildLevels(Segments);

    ASSERT_EQ(Batches.Num(), 2);
    EXPECT_TRUE(Algo::Contains(Batches[0].Members, (uint16)IndexOf(Stage, S.Writer)));
    EXPECT_TRUE(Algo::Contains(Batches[0].Members, (uint16)IndexOf(Stage, S.Reader))) << "the writer and the reader touch nothing in common";
    EXPECT_EQ(Batches[1].Members, (TVector<uint16>{ (uint16)IndexOf(Stage, S.TransformReader) })) << "reading what the writer writes waits for it";

    const FString Report = SystemSchedule::DescribeParallelism(Stage, Segments, EUpdateStage::PrePhysics);
    EXPECT_TRUE(Report.Contains("batch widths 2 1, critical path 2, widest 2")) << *Report;
}

TEST(SystemSchedule, AnExclusiveSystemAChainWaitsOnRunsBeforeUnrelatedSharedWork)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    // Reader, After and Last are a declared chain, so running the exclusive head late would stack the chain behind the writer pair.
    S.Exclusive->RunsBeforeSystem("CEntitySystemReaderTest");
    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Writer, S.TransformReader, S.Reader, S.After, S.Last, S.Exclusive }, EUpdateStage::PrePhysics);

    EXPECT_EQ(Stage.Systems[0], S.Exclusive);
    EXPECT_EQ(Levels(Stage, AllWork(Stage)).Num(), 5) << "the writer pair overlaps the chain instead of preceding the exclusive head";
}

TEST(SystemSchedule, ADependencyCycleStillSchedulesEverySystem)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.Cycle, S.Last, S.After, S.Reader }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.Num(), 4);
    for (const CEntitySystem* System : { S.Reader, S.After, S.Last, S.Cycle })
    {
        EXPECT_NE(IndexOf(Stage, System), Constants::kIndexNone);
    }

    const TVector<CWorld::FSystemBatch> Batches = Levels(Stage, AllWork(Stage));
    size_t Scheduled = 0;
    for (const CWorld::FSystemBatch& Batch : Batches)
    {
        Scheduled += Batch.Members.Num();
    }
    EXPECT_EQ(Scheduled, 4u) << "breaking the cycle must not drop a system from the stage";
}

TEST(SystemSchedule, DependenciesOnlyApplyBetweenSystemsInTheSameStage)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.After }, EUpdateStage::PrePhysics);
    ASSERT_EQ(Stage.Systems.Num(), 1) << "a dependency on a system absent from the stage is ignored";
    EXPECT_TRUE(SystemSchedule::DeclaresOrder(S.Reader, S.After));
    EXPECT_FALSE(SystemSchedule::DeclaresOrder(S.After, S.Reader));
}

TEST(SystemSchedule, ADataConflictOrdersSystemsInsideOneSegment)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.TransformReader, S.Writer, S.Reader }, EUpdateStage::PrePhysics);
    const TVector<CWorld::FSystemSegment> Segments = SystemSchedule::BuildSegments(Stage, AllWork(Stage));

    ASSERT_EQ(Segments.Num(), 1) << "a conflict only orders, so no flush splits the stage";
    const CWorld::FSystemSegment& Segment = Segments[0];
    auto LocalOf = [&](const CEntitySystem* System) { return Segment.Members.Find((uint16)IndexOf(Stage, System)); };

    const int32 Writer = LocalOf(S.Writer);
    const int32 TransformReader = LocalOf(S.TransformReader);
    EXPECT_EQ(Segment.PredecessorCounts[Writer], 0);
    EXPECT_EQ(Segment.PredecessorCounts[LocalOf(S.Reader)], 0) << "the unrelated reader starts at once";
    EXPECT_EQ(Segment.PredecessorCounts[TransformReader], 1);
    EXPECT_TRUE(Algo::Contains(Segment.Successors[Writer], (uint16)TransformReader));
}

TEST(SystemSchedule, ADeclaredOrderFlushesBetweenTheTwoSystems)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage({ S.After, S.Reader, S.Writer, S.TransformReader }, EUpdateStage::PrePhysics);
    const TVector<CWorld::FSystemSegment> Segments = SystemSchedule::BuildSegments(Stage, AllWork(Stage));

    EXPECT_EQ(SegmentOf(Segments, IndexOf(Stage, S.Reader)), 0);
    EXPECT_EQ(SegmentOf(Segments, IndexOf(Stage, S.After)), 1) << "RunsAfter sees what the earlier system spawned, so a flush comes first";
    EXPECT_EQ(SegmentOf(Segments, IndexOf(Stage, S.Writer)), SegmentOf(Segments, IndexOf(Stage, S.TransformReader)))
        << "a plain data conflict never adds a flush";
}

TEST(SystemSchedule, AnExclusiveSystemRunsAloneBetweenFlushes)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Exclusive, S.Reader };
    Stage.Precedes = TVector<uint8>(9, 0);
    const TVector<CWorld::FSystemSegment> Segments = SystemSchedule::BuildSegments(Stage, AllWork(Stage));

    ASSERT_EQ(Segments.Num(), 3);
    EXPECT_TRUE(Segments[1].bExclusive);
    ASSERT_EQ(Segments[1].Members.Num(), 1);
    EXPECT_EQ(Segments[1].Members[0], 1);
    EXPECT_FALSE(Segments[0].bExclusive);
    EXPECT_FALSE(Segments[2].bExclusive);
}

TEST(SystemSchedule, OnlyDeclaredOrderEventsAndExclusiveSystemsNeedAFlush)
{
    FSystemAccess Writer;
    Writer.Write<STransformComponent>();
    FSystemAccess Reader;
    Reader.Read<STransformComponent>();
    EXPECT_FALSE(SystemSchedule::NeedsFlushBetween(Writer, Reader, false)) << "a data conflict only orders";
    EXPECT_TRUE(SystemSchedule::NeedsFlushBetween(Writer, Reader, true));

    FSystemAccess Sender = Writer;
    Sender.SentEvents.Add(1234u);
    FSystemAccess Listener;
    Listener.Reads.Add(1234u);
    EXPECT_TRUE(SystemSchedule::NeedsFlushBetween(Sender, Listener, false)) << "an event reaches its reader at the flush";

    EXPECT_TRUE(SystemSchedule::NeedsFlushBetween(FSystemAccess::Exclusive(), Reader, false));
}

TEST(SystemSchedule, AConflictingPairWithNoDeclaredOrderIsReportedAsAmbiguous)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.TransformReader, S.Reader };
    Stage.Precedes = TVector<uint8>(9, 0);

    const TVector<SystemSchedule::FAmbiguity> Ambiguities = SystemSchedule::FindAmbiguities(Stage);
    ASSERT_EQ(Ambiguities.Num(), 1) << "only the writer and the transform reader conflict";
    EXPECT_EQ(Ambiguities[0].First, 0);
    EXPECT_EQ(Ambiguities[0].Second, 1);
    EXPECT_EQ(Ambiguities[0].ConflictId, static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()));
    EXPECT_NE(GetAccessTypeName(Ambiguities[0].ConflictId), NAME_None) << "the log names what the pair conflicts on";

    Stage.Precedes[0 * 3 + 1] = 1;
    EXPECT_TRUE(SystemSchedule::FindAmbiguities(Stage).IsEmpty()) << "a declared order settles the conflict";
}

TEST(SystemSchedule, ExclusiveSystemsAreNeverReportedAsAmbiguous)
{
    FScheduleSystems S;
    ASSERT_TRUE(S.AllCreated());

    CWorld::FStageSchedule Stage;
    Stage.Systems  = { S.Writer, S.Exclusive };
    Stage.Precedes = TVector<uint8>(4, 0);
    EXPECT_TRUE(SystemSchedule::FindAmbiguities(Stage).IsEmpty());
}

TEST(SystemSchedule, TheEngineTransformSystemsBeforePhysicsDeclareTheirOrder)
{
    RegisterPendingTypes();

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    EntitySystems::CreateMissing(*World, {}, World->Systems);

    TVector<CEntitySystem*> InStage;
    for (const TStrongObjectPtr<CEntitySystem>& System : World->Systems)
    {
        if (System->RunsInStage(EUpdateStage::PrePhysics))
        {
            InStage.Add(System.Get());
        }
    }

    const FName Ordered[] = { "CMovementComponentSystem", "CProjectileSystem", "CPathFollowSystem", "CBuoyancySystem",
        "CFoliageCollisionSystem", "CRagdollSystem", "CSocketAttachmentSystem", "CRVOAvoidanceSystem" };
    const CWorld::FStageSchedule Stage = SystemSchedule::OrderStage(InStage, EUpdateStage::PrePhysics);
    for (const SystemSchedule::FAmbiguity& Ambiguity : SystemSchedule::FindAmbiguities(Stage))
    {
        const FName First = Stage.Systems[Ambiguity.First]->GetClass()->GetName();
        const FName Second = Stage.Systems[Ambiguity.Second]->GetClass()->GetName();
        EXPECT_FALSE(Algo::Contains(Ordered, First) && Algo::Contains(Ordered, Second))
            << *First.ToString() << " and " << *Second.ToString() << " race on " << *GetAccessTypeName(Ambiguity.ConflictId).ToString();
    }
    EntitySystems::DestroyAll(SSystemContext(), World->Systems);
}

TEST(SystemSchedule, TheEngineSystemSetReportsEverySystemItSchedules)
{
    RegisterPendingTypes();

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    EntitySystems::CreateMissing(*World, {}, World->Systems);
    FWorldTestAccess::RebuildSystemSchedule(*World);

    // Flushed first, so the ambiguity lines the rebuild logged do not interleave with the report.
    Logging::Flush();
    const FString Report = World->DescribeSystemParallelism(true);
    fprintf(stderr, "%s", *Report);
    fflush(stderr);
    for (const TStrongObjectPtr<CEntitySystem>& System : World->Systems)
    {
        const FString Name = System->GetClass()->GetName().ToString();
        bool bScheduled = false;
        for (uint8 Stage = 0; Stage < (uint8)EUpdateStage::Max; ++Stage)
        {
            bScheduled |= System->RunsInStage((EUpdateStage)Stage);
        }
        EXPECT_TRUE(!bScheduled || Report.Contains(Name)) << *Name;
    }
    EntitySystems::DestroyAll(SSystemContext(), World->Systems);
}

TEST(SystemSchedule, LookupOnlyQueriesDoNotGiveASystemWork)
{
    RegisterPendingTypes();

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    EntitySystems::CreateMissing(*World, {}, World->Systems);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Target = Registry.Create();
    Registry.Emplace<STransformComponent>(Target);
    Registry.Emplace<SSkeletalMeshComponent>(Target);
    Registry.Emplace<SStaticMeshComponent>(Target);
    Registry.Emplace<SNavMeshComponent>(Target);
    Registry.Emplace<SAIStimuliSourceComponent>(Target);

    const SSystemContext Context = FWorldTestAccess::SystemContext(*World);
    auto FindSystem = [&](const char* Name) { return EntitySystems::Find(World->Systems, FindObject<CClass>(FName(Name))); };
    const char* LookupOnly[] = { "CAnimationSystem", "CAudioSystem", "CCameraRigSystem", "CPathFollowSystem",
        "CPerceptionSystem", "CRVOAvoidanceSystem", "CRagdollSystem", "CSocketAttachmentSystem" };
    for (const char* Name : LookupOnly)
    {
        CEntitySystem* System = FindSystem(Name);
        ASSERT_NE(System, nullptr) << Name;
        EXPECT_FALSE(System->HasWork(Context)) << Name;
    }

    CEntitySystem* Rig = FindSystem("CCameraRigSystem");
    const ECS::FEntity Follower = Registry.Create();
    Registry.Emplace<STransformComponent>(Follower);
    Registry.Emplace<SCameraFollowComponent>(Follower).Target = Target;
    EXPECT_TRUE(Rig->HasWork(Context)) << "a rig is what drives the system";

    EntitySystems::DestroyAll(SSystemContext(), World->Systems);
}
