#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#include "Scripting/ScriptableTest.h"
#include "World/World.h"
#include "World/Subsystems/NetSubsystem.h"
#include "World/Subsystems/WorldSubsystem.h"

using namespace Lumina;

namespace
{
    // Never initialized, so the engine's own subsystems find no world resources and stay inert.
    CWorld& BareWorld()
    {
        static CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        return *World;
    }

    // Flips the gate for one test and puts it back, so discovery in any other test still skips the type.
    struct FCreationGate
    {
        FCreationGate()
        {
            // The harness only starts the object system, so the deferred class pass has to run here.
            RegisterPendingTypes();
            CWorldSubsystemTest::bAllowCreation = true;
        }

        ~FCreationGate() { CWorldSubsystemTest::bAllowCreation = false; }
    };

    CWorldSubsystemTest* FindTestSubsystem(const TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        return Cast<CWorldSubsystemTest>(
            WorldSubsystems::Find(Subsystems, CWorldSubsystemTest::StaticClass()));
    }
}

TEST(WorldSubsystem, CreateMissingHonorsShouldCreate)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(BareWorld(), Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr) << "a subsystem class that wants to exist was not created";

    EXPECT_EQ(Created->GetWorld(), &BareWorld()) << "the owning world is set before OnInitialize runs";

    EXPECT_EQ(WorldSubsystems::Find(Subsystems, CWorldSubsystemDeclineTest::StaticClass()), nullptr)
        << "a class whose ShouldCreate returns false must be skipped";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, InitializeRunsBeforeWorldReady)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(BareWorld(), Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    EXPECT_EQ(Created->InitializeCount, 1);
    EXPECT_EQ(Created->ReadyCount, 1);
    EXPECT_TRUE(Created->bReadySawInitialize) << "OnWorldReady must run after every OnInitialize";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, CreateMissingIsIdempotent)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    const int32 First = WorldSubsystems::CreateMissing(BareWorld(), Subsystems);
    ASSERT_GT(First, 0);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    const int32 CountAfterFirst = Subsystems.Num();

    EXPECT_EQ(WorldSubsystems::CreateMissing(BareWorld(), Subsystems), 0)
        << "a second pass must not duplicate a subsystem that already exists";
    EXPECT_EQ(Subsystems.Num(), CountAfterFirst);
    EXPECT_EQ(Created->InitializeCount, 1) << "an existing subsystem must not be initialized twice";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, UpdateAndTeardownReachEverySubsystem)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(BareWorld(), Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    WorldSubsystems::Update(Subsystems, 0.25f);
    WorldSubsystems::Update(Subsystems, 0.25f);

    EXPECT_EQ(Created->UpdateCount, 2);
    EXPECT_FLOAT_EQ(Created->AccumulatedTime, 0.5f);

    WorldSubsystems::DestroyAll(Subsystems);

    EXPECT_EQ(Created->TeardownCount, 1);
    EXPECT_EQ(Created->GetWorld(), nullptr) << "teardown must unlink the world it no longer belongs to";
    EXPECT_TRUE(Subsystems.IsEmpty());
}

TEST(WorldSubsystem, DropScriptedLeavesNativeSubsystemsAlone)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(BareWorld(), Subsystems);

    const int32 Before = Subsystems.Num();
    ASSERT_GT(Before, 0u);

    // Nothing here is backed by a script class, so a reload rebuild must not disturb the native ones.
    WorldSubsystems::DropScripted(Subsystems);

    EXPECT_EQ(Subsystems.Num(), Before);
    EXPECT_NE(FindTestSubsystem(Subsystems), nullptr);

    WorldSubsystems::DestroyAll(Subsystems);
}

// Session events reach CNetSubsystem's delegates when the world dispatches them, in send order and once each.
TEST(WorldSubsystem, NetSessionEventsBroadcastThroughDelegates)
{
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    CNetSubsystem* Net = NewObject<CNetSubsystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    Net->SetOwningWorld(World);

    FString Log;
    Net->OnClientJoined.AddLambda([&Log](uint32 ConnectionId) { Log += Lumina::Format("C{} ", ConnectionId); });
    Net->OnClientLeft.AddLambda([&Log](uint32 ConnectionId) { Log += Lumina::Format("D{} ", ConnectionId); });
    Net->OnJoinedHost.AddLambda([&Log]() { Log += "J "; });
    Net->OnLeftHost.AddLambda([&Log](ENetLeaveReason Reason) { Log += Lumina::Format("L{} ", (uint32)Reason); });

    const ENetSessionEventKind Kinds[] = { ENetSessionEventKind::ClientJoined, ENetSessionEventKind::ClientLeft,
                                           ENetSessionEventKind::JoinedHost, ENetSessionEventKind::LeftHost };
    for (const ENetSessionEventKind Kind : Kinds)
    {
        SNetSessionEvent Event;
        Event.Kind         = Kind;
        Event.ConnectionId = 3;
        Event.Reason       = ENetLeaveReason::HostClosed;
        ECS::SendEvent(World->GetEventQueues(), Event);
    }

    Net->DispatchSessionEvents();
    EXPECT_TRUE(Log.IsEmpty()) << "an event is only readable once its queue committed";

    World->GetEventQueues().Commit();
    Net->DispatchSessionEvents();
    EXPECT_EQ(Log, FString("C3 D3 J L3 "));

    Net->DispatchSessionEvents();
    EXPECT_EQ(Log, FString("C3 D3 J L3 ")) << "each event is told once";
}
