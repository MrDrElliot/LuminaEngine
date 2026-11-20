#include <gtest/gtest.h>

#include "Core/Math/Random.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Memory/Memcpy.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptableTest.h"
#include "Simulation/DeterministicMath.h"
#include "Simulation/Simulation.h"
#include "Simulation/SimulationLibrary.h"
#include <cmath>
#include "Simulation/SimulationComponents.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    // What each player presses at a tick, varied enough that a repeat-last prediction is often wrong.
    SSimTestInput ScriptedInput(uint32 Player, int64 Tick)
    {
        SSimTestInput Input;
        Input.MoveX = Player == 0 ? 1 : (int32)((Tick / 7) % 3) - 1;
        Input.bFire = Player == 0 ? Tick % 10 == 0 : Tick % 13 == 0;
        return Input;
    }

    struct FMessage
    {
        uint32       To = 0;
        uint32       From = 0;
        int32        DeliverFrame = 0;
        bool         bChecksum = false;
        FSimInputRun Inputs;
        FSimChecksum Checksum;
    };

    struct FPeer
    {
        CWorld*                  World = nullptr;
        TStrongObjectPtr<CWorld> Owner;
        FSimulation*             Sim = nullptr;
        CSimTestSystem*          System = nullptr;
        // Submits through the C# WorldSimulation instead, returning one when the input landed.
        int32 (*ManagedSubmit)(void* World, int32 MoveX, int32 bFire) = nullptr;
    };

    // Peers in one process, joined by a network that delays every message and drops some, so rollback and recovery both run.
    struct FSimHarness
    {
        TVector<FPeer>    Peers;
        TVector<FMessage> InFlight;
        FRandomStream     Network{ 1234 };
        int32             Frame = 0;
        int32             MinLagFrames = 0;
        int32             MaxLagFrames = 0;
        uint32            DropPercent = 0;

        explicit FSimHarness(uint32 NumPlayers, FSimSessionDesc Base)
        {
            RegisterPendingTypes();
            CEntitySystemTest::bAllowCreation = true;
            for (uint32 Player = 0; Player < NumPlayers; ++Player)
            {
                FPeer& Peer = Peers.EmplaceGetRef();
                Peer.World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
                Peer.Owner = Peer.World;
                FWorldTestAccess::KeepOnlySystems(*Peer.World, { FName("CSimTestSystem") });
                for (const TStrongObjectPtr<CEntitySystem>& System : Peer.World->Systems)
                {
                    if (CSimTestSystem* Found = Cast<CSimTestSystem>(System.Get()))
                    {
                        Peer.System = Found;
                    }
                }
                Peer.Sim = &FWorldTestAccess::EnableSimulation(*Peer.World);

                FSimSessionDesc Desc = Base;
                Desc.NumPlayers  = NumPlayers;
                Desc.LocalPlayer = Player;
                Desc.Seed        = 77;
                Desc.InputStruct = SSimTestInput::StaticStruct();
                EXPECT_TRUE(Peer.Sim->Start(Desc));
            }
            CEntitySystemTest::bAllowCreation = false;
        }

        ~FSimHarness()
        {
            for (FPeer& Peer : Peers)
            {
                Peer.Sim->Stop();
                EntitySystems::DestroyAll(SSystemContext(), Peer.World->Systems);
            }
        }

        void Send(uint32 From, FMessage&& Message)
        {
            for (uint32 To = 0; To < (uint32)Peers.Num(); ++To)
            {
                if (To == From || Network.NextUInt32() % 100 < DropPercent)
                {
                    continue;
                }
                FMessage Copy = Message;
                Copy.From = From;
                Copy.To = To;
                Copy.DeliverFrame = Frame + MinLagFrames + (MaxLagFrames > MinLagFrames ? (int32)(Network.NextUInt32() % (uint32)(MaxLagFrames - MinLagFrames + 1)) : 0);
                InFlight.Add(Move(Copy));
            }
        }

        void Step(bool bSubmitInputs = true)
        {
            constexpr double kFrameSeconds = 1.0 / 60.0;
            for (uint32 Player = 0; Player < (uint32)Peers.Num(); ++Player)
            {
                FSimulation& Sim = *Peers[Player].Sim;
                if (bSubmitInputs)
                {
                    // Submitted for the tick it will be stamped on, the way a game samples its controls each frame.
                    const int64 StampTick = (int64)Sim.GetCurrentTick() + 2;
                    const SSimTestInput Input = ScriptedInput(Player, StampTick);
                    if (Peers[Player].ManagedSubmit != nullptr)
                    {
                        EXPECT_EQ(Peers[Player].ManagedSubmit(Peers[Player].World, Input.MoveX, Input.bFire ? 1 : 0), 1);
                    }
                    else
                    {
                        Sim.SubmitLocalInput(Input);
                    }
                }
                Sim.Advance(kFrameSeconds);

                TVector<FSimInputRun> Runs;
                Sim.TakeOutgoingInputs(Runs);
                for (FSimInputRun& Run : Runs)
                {
                    FMessage Message;
                    Message.Inputs = Move(Run);
                    Send(Player, Move(Message));
                }
                TVector<FSimChecksum> Sums;
                Sim.TakeOutgoingChecksums(Sums);
                for (FSimChecksum& Sum : Sums)
                {
                    FMessage Message;
                    Message.bChecksum = true;
                    Message.Checksum = Move(Sum);
                    Send(Player, Move(Message));
                }
            }

            TVector<FMessage> Later;
            for (FMessage& Message : InFlight)
            {
                if (Message.DeliverFrame > Frame)
                {
                    Later.Add(Move(Message));
                    continue;
                }
                FSimulation& Sim = *Peers[Message.To].Sim;
                Message.bChecksum ? Sim.ReceiveChecksum(Message.From, Message.Checksum) : Sim.ReceiveInputs(Message.Inputs);
            }
            InFlight = Move(Later);
            ++Frame;
        }
    };

    FSimSessionDesc RollbackDesc()
    {
        FSimSessionDesc Desc;
        Desc.Mode             = ESimulationMode::Rollback;
        Desc.TickRate         = 60;
        Desc.InputDelayTicks  = 2;
        Desc.MaxRollbackTicks = 10;
        Desc.ChecksumInterval = 5;
        return Desc;
    }
}

// Two peers a few frames apart predict each other, rewind on every wrong guess, and still agree on every confirmed tick.
TEST(Simulation, RollbackPeersStayInSyncThroughLagAndLoss)
{
    FSimHarness Harness(2, RollbackDesc());
    Harness.MinLagFrames = 2;
    Harness.MaxLagFrames = 6;
    Harness.DropPercent  = 10;

    for (int32 i = 0; i < 600; ++i)
    {
        Harness.Step();
    }

    for (const FPeer& Peer : Harness.Peers)
    {
        EXPECT_FALSE(Peer.Sim->HasDesynced()) << "desync at tick " << Peer.Sim->GetDesyncTick();
        EXPECT_GT(Peer.Sim->GetRollbackCount(), 0u) << "the lag should have forced rewinds";
        EXPECT_GT(Peer.Sim->GetVerifiedChecksums(), 50u) << "checksums should have been compared all along";
        EXPECT_GT(Peer.Sim->GetConfirmedTick(), 500);
        EXPECT_GT(Peer.System->ProjectilesSpawned, 0);
    }
}

// Three peers with heavy jitter exercise prediction for two remote players at once.
TEST(Simulation, ThreePlayerRollbackStaysInSync)
{
    FSimHarness Harness(3, RollbackDesc());
    Harness.MinLagFrames = 1;
    Harness.MaxLagFrames = 8;
    Harness.DropPercent  = 20;

    for (int32 i = 0; i < 600; ++i)
    {
        Harness.Step();
    }

    for (const FPeer& Peer : Harness.Peers)
    {
        EXPECT_FALSE(Peer.Sim->HasDesynced()) << "desync at tick " << Peer.Sim->GetDesyncTick();
        EXPECT_GT(Peer.Sim->GetVerifiedChecksums(), 50u);
    }
}

// Lockstep never predicts, so it never rewinds, and it waits out the lag instead.
TEST(Simulation, LockstepWaitsForInputsAndNeverRewinds)
{
    FSimSessionDesc Desc = RollbackDesc();
    Desc.Mode = ESimulationMode::Lockstep;
    FSimHarness Harness(2, Desc);
    Harness.MinLagFrames = 3;
    Harness.MaxLagFrames = 3;

    for (int32 i = 0; i < 400; ++i)
    {
        Harness.Step();
    }

    for (const FPeer& Peer : Harness.Peers)
    {
        EXPECT_FALSE(Peer.Sim->HasDesynced());
        EXPECT_EQ(Peer.Sim->GetRollbackCount(), 0u);
        EXPECT_GT(Peer.Sim->GetVerifiedChecksums(), 20u);
        // A one way lag of three frames outruns a two tick input delay, so lockstep waits part of the time but keeps going.
        EXPECT_GT(Peer.Sim->GetCurrentTick(), 250u);
        EXPECT_LT(Peer.Sim->GetCurrentTick(), 400u);
    }
}

// A peer whose state drifts once is caught by the next checksum exchange that covers that tick.
TEST(Simulation, ChecksumsCatchADesync)
{
    FSimHarness Harness(2, RollbackDesc());
    Harness.MinLagFrames = 1;
    Harness.MaxLagFrames = 3;
    Harness.Peers[1].System->CorruptAtTick = 100;

    for (int32 i = 0; i < 300; ++i)
    {
        Harness.Step();
    }

    for (const FPeer& Peer : Harness.Peers)
    {
        ASSERT_TRUE(Peer.Sim->HasDesynced());
        EXPECT_GT(Peer.Sim->GetDesyncTick(), 100);
        EXPECT_LE(Peer.Sim->GetDesyncTick(), 110);
    }
}

// The same inputs replayed from scratch reach the same state, the property every rewind relies on.
TEST(Simulation, ReplayingTheSameInputsReachesTheSameState)
{
    FSimSessionDesc Desc = RollbackDesc();
    FSimHarness Live(1, Desc);
    FSimHarness Replay(1, Desc);
    for (int32 i = 0; i < 300; ++i)
    {
        Live.Step();
        Replay.Step();
    }
    EXPECT_EQ(Live.Peers[0].Sim->ComputeChecksum().Total, Replay.Peers[0].Sim->ComputeChecksum().Total);
    EXPECT_GT(Live.Peers[0].System->ProjectilesSpawned, 0);
}

// The simulation's own trigonometry stays close to the runtime library's without depending on it.
TEST(Simulation, DeterministicMathTracksTheLibrary)
{
    for (int32 Step = -4000; Step <= 4000; ++Step)
    {
        const float Angle = (float)Step * 0.005f;
        EXPECT_NEAR(SimMath::Sin(Angle), std::sin(Angle), 2e-6f) << Angle;
        EXPECT_NEAR(SimMath::Cos(Angle), std::cos(Angle), 2e-6f) << Angle;
    }
    for (int32 Step = -200; Step <= 200; ++Step)
    {
        const float Y = (float)Step * 0.37f;
        for (const float X : { -3.0f, -0.5f, 0.0f, 0.25f, 4.0f })
        {
            EXPECT_NEAR(SimMath::Atan2(Y, X), std::atan2(Y, X), 2e-5f) << Y << ", " << X;
        }
    }
}

// A standalone world starts a one player session by the input struct's name, the call C# makes through WorldSimulation.Start.
TEST(Simulation, AStandaloneWorldStartsASessionByInputName)
{
    RegisterPendingTypes();
    TStrongObjectPtr<CWorld> World(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    FSimulation& Sim = FWorldTestAccess::EnableSimulation(*World);

    EXPECT_FALSE(CSimulationLibrary::StartSession(World.Get(), FName("NoSuchInputStruct"), 1));
    EXPECT_FALSE(Sim.IsRunning());

    ASSERT_TRUE(CSimulationLibrary::StartSession(World.Get(), FName("SSimTestInput"), 9));
    EXPECT_TRUE(CSimulationLibrary::IsSessionRunning(World.Get()));
    EXPECT_EQ(CSimulationLibrary::GetSessionPlayers(World.Get()), 1u);
    EXPECT_EQ(Sim.GetInputStruct(), SSimTestInput::StaticStruct());
    EXPECT_NE(CSimulationLibrary::GetLocalInput(World.Get(), FName("SSimTestInput")), nullptr);
    EXPECT_EQ(CSimulationLibrary::GetLocalInput(World.Get(), FName("SSimTestBody")), nullptr) << "the wrong struct is refused, not reinterpreted";
    Sim.Stop();
}

class FManagedSimulationTest : public ::testing::Test
{
public:

    static void SetUpTestSuite()
    {
        Paths::InitializePaths();
        DotNet::Initialize();
    }

    static void TearDownTestSuite()
    {
        if (DotNet::IsInitialized())
        {
            DotNet::Shutdown();
        }
    }

    void SetUp() override
    {
        if (!DotNet::IsInitialized())
        {
            GTEST_SKIP() << "the managed host did not start in this process";
        }
    }
};

// One peer simulates in C++ and the other in C#, through lag, loss and rewinds, and the checksums hold them to the same state.
TEST_F(FManagedSimulationTest, ACSharpPeerAgreesWithACppPeer)
{
    auto* Tick   = (void(*)(const SExclusiveContext*))DotNet::ResolveManagedExport("Test_SimulationTick");
    auto* Submit = (int32(*)(void*, int32, int32))DotNet::ResolveManagedExport("Test_SimulationSubmit");
    ASSERT_NE(Tick, nullptr);
    ASSERT_NE(Submit, nullptr);

    FSimHarness Harness(2, RollbackDesc());
    Harness.MinLagFrames = 2;
    Harness.MaxLagFrames = 6;
    Harness.DropPercent  = 10;
    Harness.Peers[1].System->ManagedTick = Tick;
    Harness.Peers[1].ManagedSubmit = Submit;

    for (int32 i = 0; i < 600; ++i)
    {
        Harness.Step();
    }

    for (const FPeer& Peer : Harness.Peers)
    {
        EXPECT_FALSE(Peer.Sim->HasDesynced()) << "desync at tick " << Peer.Sim->GetDesyncTick();
        EXPECT_GT(Peer.Sim->GetRollbackCount(), 0u) << "the lag should have forced the C# tick to replay";
        EXPECT_GT(Peer.Sim->GetVerifiedChecksums(), 50u);
    }
    EXPECT_GT(Harness.Peers[0].System->ProjectilesSpawned, 0);
    EXPECT_EQ(Harness.Peers[1].System->ProjectilesSpawned, 0) << "the C# peer's projectiles came from C#";

    TVector<ECS::FEntity> Entities;
    Harness.Peers[1].Sim->GetEntities(Entities);
    int32 Owners = 0;
    for (ECS::FEntity Entity : Entities)
    {
        const SSimTestBody* Body = Harness.Peers[1].World->GetEntityRegistry().TryGet<SSimTestBody>(Entity);
        Owners += Body != nullptr && Body->Owner >= 0 ? 1 : 0;
    }
    EXPECT_EQ(Owners, 2) << "C# spawned one body per player";
}

// A session refuses an input of another struct from C#, and takes none at all once it stops.
TEST_F(FManagedSimulationTest, CSharpInputOfTheWrongStructIsRefused)
{
    auto* Submit    = (int32(*)(void*, int32, int32))DotNet::ResolveManagedExport("Test_SimulationSubmit");
    auto* WrongType = (int32(*)(void*))DotNet::ResolveManagedExport("Test_SimulationSubmitWrongType");
    ASSERT_NE(Submit, nullptr);
    ASSERT_NE(WrongType, nullptr);

    FSimHarness Harness(1, RollbackDesc());
    FPeer& Peer = Harness.Peers[0];
    EXPECT_EQ(WrongType(Peer.World), 0);
    ASSERT_EQ(Submit(Peer.World, -1, 1), 1);
    const SSimTestInput* Held = static_cast<const SSimTestInput*>(Peer.Sim->GetLocalInput());
    EXPECT_EQ(Held->MoveX, -1);
    EXPECT_TRUE(Held->bFire);

    Peer.Sim->Stop();
    EXPECT_EQ(Submit(Peer.World, 1, 0), 0);
}

// The C# SimMath runs the same operations in the same order as the C++ one, so both land on the same bits.
TEST_F(FManagedSimulationTest, CSharpSimMathMatchesCppBitForBit)
{
    auto* Evaluate = (void(*)(float, float, float*))DotNet::ResolveManagedExport("Test_SimMathEvaluate");
    ASSERT_NE(Evaluate, nullptr);

    int32 Mismatches = 0;
    for (int32 Step = -3000; Step <= 3000; ++Step)
    {
        const float A = (float)Step * 0.0137f;
        const float B = (float)((Step * 7919) % 401 - 200) * 0.031f;
        float Managed[5] = {};
        Evaluate(A, B, Managed);
        const float Native[5] = { SimMath::Sin(A), SimMath::Cos(A), SimMath::Atan(A), SimMath::Atan2(A, B), SimMath::WrapAngle(A) };
        for (int32 i = 0; i < 5; ++i)
        {
            Mismatches += Memory::Memcmp(&Managed[i], &Native[i], sizeof(float)) != 0 ? 1 : 0;
        }
    }
    EXPECT_EQ(Mismatches, 0);
}
