#include "RuntimePCH.h"
#include "NetSimulation.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Simulation/Simulation.h"
#include "World/World.h"

namespace Lumina
{
    namespace NetSimulationInternal
    {
        // Simulation messages reuse the reliable channel for setup and checksums and the unreliable one for inputs, which repeat themselves.
        constexpr uint8 kReliableChannel   = 0;
        constexpr uint8 kUnreliableChannel = 1;

        void WriteInputs(TVector<uint8>& Out, uint32 Player, const FSimInputRun& Run, uint32 InputSize)
        {
            FNetArchive Writer(Out);
            uint8  Type      = static_cast<uint8>(ENetMessage::SimInput);
            uint8  Slot      = static_cast<uint8>(Player);
            uint32 FirstTick = Run.FirstTick;
            uint8  NumTicks  = static_cast<uint8>(Run.NumTicks);
            uint16 Size      = static_cast<uint16>(InputSize);
            Writer << Type << Slot << FirstTick << NumTicks << Size;
            Writer.Serialize(const_cast<uint8*>(Run.Bytes.GetData()), Run.Bytes.Num());
        }

        void WriteChecksum(TVector<uint8>& Out, uint32 Player, const FSimChecksum& Checksum)
        {
            FNetArchive Writer(Out);
            uint8  Type  = static_cast<uint8>(ENetMessage::SimChecksum);
            uint8  Slot  = static_cast<uint8>(Player);
            uint32 Tick  = Checksum.Tick;
            uint64 Total = Checksum.Total;
            uint8  Count = static_cast<uint8>(Math::Min(Checksum.PerComponent.Num(), 255));
            Writer << Type << Slot << Tick << Total << Count;
            for (uint8 i = 0; i < Count; ++i)
            {
                uint64 Value = Checksum.PerComponent[i];
                Writer << Value;
            }
        }

        // Every ready client but the one a message came from.
        template<typename TFunc>
        void ForEachSimClient(const FNetWorldState& State, uint32 Except, TFunc&& Func)
        {
            for (const auto& [Connection, Player] : State.SimPlayerOfConnection)
            {
                if (Connection != Except)
                {
                    Func(Connection);
                }
            }
        }
    }

    bool NetSimulation::StartSession(CWorld* World, FNetWorldState& State, const CStruct* InputStruct, uint64 Seed)
    {
        using namespace NetSimulationInternal;

        if (World == nullptr || State.Transport == nullptr || InputStruct == nullptr)
        {
            return false;
        }

        const bool bHostPlays = World->GetNetMode() == ENetMode::ListenServer;
        FSimSessionDesc Desc = FSimSessionDesc::FromProjectSettings();
        Desc.InputStruct = InputStruct;
        Desc.Seed        = Seed;

        TVector<uint32> Clients = State.ReadyClientIds;
        Algo::Sort(Clients);
        Desc.NumPlayers = (uint32)Clients.Num() + (bHostPlays ? 1u : 0u);
        if (Desc.NumPlayers == 0 || Desc.NumPlayers > 255)
        {
            LOG_ERROR("[Net][Sim] A session needs between 1 and 255 players, and this one has {}.", Desc.NumPlayers);
            return false;
        }

        State.SimPlayerOfConnection.Reset();
        const FString StructName = InputStruct->GetName().ToString();
        for (int32 Index = 0; Index < Clients.Num(); ++Index)
        {
            const uint32 Player = (uint32)Index + (bHostPlays ? 1u : 0u);
            State.SimPlayerOfConnection.Add(Clients[Index], Player);

            TVector<uint8> Message;
            FNetArchive Writer(Message);
            uint8  Type       = static_cast<uint8>(ENetMessage::SimStart);
            uint64 SeedValue  = Seed;
            uint8  NumPlayers = static_cast<uint8>(Desc.NumPlayers);
            uint8  Slot       = static_cast<uint8>(Player);
            uint8  Mode       = static_cast<uint8>(Desc.Mode);
            uint16 TickRate   = static_cast<uint16>(Desc.TickRate);
            uint8  Delay      = static_cast<uint8>(Desc.InputDelayTicks);
            uint8  Rollback   = static_cast<uint8>(Desc.MaxRollbackTicks);
            uint16 Checksums  = static_cast<uint16>(Desc.ChecksumInterval);
            uint16 NameLength = static_cast<uint16>(StructName.Len());
            Writer << Type << SeedValue << NumPlayers << Slot << Mode << TickRate << Delay << Rollback << Checksums << NameLength;
            Writer.Serialize(const_cast<char*>(*StructName), NameLength);
            Net::SendFramed(*State.Transport, FConnectionHandle{ Clients[Index] }, Message.GetData(), (SIZE_T)Message.Num(), kReliableChannel, ESendMode::Reliable);
        }

        if (bHostPlays)
        {
            FSimulation* Sim = World->GetSimulation();
            if (Sim == nullptr)
            {
                LOG_ERROR("[Net][Sim] The host world has no simulation. Enable Deterministic Simulation in the project settings.");
                return false;
            }
            Desc.LocalPlayer = 0;
            return Sim->Start(Desc);
        }
        LOG_INFO("[Net][Sim] Dedicated host relaying a {} player session.", Desc.NumPlayers);
        return true;
    }

    void NetSimulation::Pump(CWorld* World, FNetWorldState& State, bool bServer)
    {
        using namespace NetSimulationInternal;

        FSimulation* Sim = World != nullptr ? World->GetSimulation() : nullptr;
        if (Sim == nullptr || !Sim->IsRunning() || State.Transport == nullptr)
        {
            return;
        }

        TVector<FSimInputRun> Runs;
        Sim->TakeOutgoingInputs(Runs);
        TVector<FSimChecksum> Checksums;
        Sim->TakeOutgoingChecksums(Checksums);

        TVector<uint8> Message;
        for (const FSimInputRun& Run : Runs)
        {
            Message.Reset();
            WriteInputs(Message, Sim->GetLocalPlayer(), Run, Sim->GetInputSize());
            if (bServer)
            {
                ForEachSimClient(State, 0u, [&](uint32 Connection)
                {
                    Net::SendFramed(*State.Transport, FConnectionHandle{ Connection }, Message.GetData(), (SIZE_T)Message.Num(), kUnreliableChannel, ESendMode::Unreliable);
                });
            }
            else
            {
                Net::SendFramed(*State.Transport, State.ServerConnection, Message.GetData(), (SIZE_T)Message.Num(), kUnreliableChannel, ESendMode::Unreliable);
            }
        }

        for (const FSimChecksum& Checksum : Checksums)
        {
            Message.Reset();
            WriteChecksum(Message, Sim->GetLocalPlayer(), Checksum);
            if (bServer)
            {
                ForEachSimClient(State, 0u, [&](uint32 Connection)
                {
                    Net::SendFramed(*State.Transport, FConnectionHandle{ Connection }, Message.GetData(), (SIZE_T)Message.Num(), kReliableChannel, ESendMode::Reliable);
                });
            }
            else
            {
                Net::SendFramed(*State.Transport, State.ServerConnection, Message.GetData(), (SIZE_T)Message.Num(), kReliableChannel, ESendMode::Reliable);
            }
        }
    }

    bool NetSimulation::HandleMessage(CWorld* World, FNetWorldState& State, bool bServer, uint32 SenderConnection, ENetMessage Type, const uint8* Data, SIZE_T Size)
    {
        using namespace NetSimulationInternal;

        if (Type != ENetMessage::SimStart && Type != ENetMessage::SimInput && Type != ENetMessage::SimChecksum)
        {
            return false;
        }

        FSimulation* Sim = World != nullptr ? World->GetSimulation() : nullptr;
        FNetArchive Reader(Data, Size);
        uint8 Header = 0;
        Reader << Header;

        if (Type == ENetMessage::SimStart)
        {
            uint64 Seed = 0;
            uint8  NumPlayers = 0, Slot = 0, Mode = 0, Delay = 0, Rollback = 0;
            uint16 TickRate = 0, Checksums = 0, NameLength = 0;
            Reader << Seed << NumPlayers << Slot << Mode << TickRate << Delay << Rollback << Checksums << NameLength;
            TVector<char> NameBytes;
            NameBytes.SetNum(NameLength);
            Reader.Serialize(NameBytes.GetData(), NameLength);
            const FString StructName(NameBytes.GetData(), NameLength);
            if (bServer || Reader.HasError())
            {
                return true;
            }

            const CStruct* InputStruct = FindObject<CStruct>(FName(*StructName));
            if (Sim == nullptr || InputStruct == nullptr)
            {
                LOG_ERROR("[Net][Sim] Cannot join the host's simulation: {}.",
                    Sim == nullptr ? "Deterministic Simulation is off in this project's settings" : "its input struct is not in this build");
                return true;
            }

            FSimSessionDesc Desc;
            Desc.Seed             = Seed;
            Desc.NumPlayers       = NumPlayers;
            Desc.LocalPlayer      = Slot;
            Desc.Mode             = static_cast<ESimulationMode>(Mode);
            Desc.TickRate         = TickRate;
            Desc.InputDelayTicks  = Delay;
            Desc.MaxRollbackTicks = Rollback;
            Desc.ChecksumInterval = Checksums;
            Desc.InputStruct      = InputStruct;
            Sim->Start(Desc);
            return true;
        }

        // A client's slot comes from the host's table, never from what the client wrote.
        uint8 ClaimedSlot = 0;
        Reader << ClaimedSlot;
        uint32 Player = ClaimedSlot;
        if (bServer)
        {
            const uint32* Assigned = State.SimPlayerOfConnection.Find(SenderConnection);
            if (Assigned == nullptr)
            {
                return true;
            }
            Player = *Assigned;
        }

        if (Type == ENetMessage::SimInput)
        {
            FSimInputRun Run;
            uint8  NumTicks = 0;
            uint16 InputSize = 0;
            Run.Player = Player;
            Reader << Run.FirstTick << NumTicks << InputSize;
            Run.NumTicks = NumTicks;
            Run.Bytes.SetNum((int32)NumTicks * InputSize);
            Reader.Serialize(Run.Bytes.GetData(), Run.Bytes.Num());
            if (Reader.HasError())
            {
                return true;
            }

            if (Sim != nullptr && Sim->IsRunning() && InputSize == Sim->GetInputSize())
            {
                Sim->ReceiveInputs(Run);
            }
            if (bServer)
            {
                TVector<uint8> Relay;
                WriteInputs(Relay, Player, Run, InputSize);
                ForEachSimClient(State, SenderConnection, [&](uint32 Connection)
                {
                    Net::SendFramed(*State.Transport, FConnectionHandle{ Connection }, Relay.GetData(), (SIZE_T)Relay.Num(), kUnreliableChannel, ESendMode::Unreliable);
                });
            }
            return true;
        }

        FSimChecksum Checksum;
        uint8 Count = 0;
        Reader << Checksum.Tick << Checksum.Total << Count;
        for (uint8 i = 0; i < Count; ++i)
        {
            uint64 Value = 0;
            Reader << Value;
            Checksum.PerComponent.Add(Value);
        }
        if (Reader.HasError())
        {
            return true;
        }

        if (Sim != nullptr && Sim->IsRunning())
        {
            Sim->ReceiveChecksum(Player, Checksum);
        }
        if (bServer)
        {
            TVector<uint8> Relay;
            WriteChecksum(Relay, Player, Checksum);
            ForEachSimClient(State, SenderConnection, [&](uint32 Connection)
            {
                Net::SendFramed(*State.Transport, FConnectionHandle{ Connection }, Relay.GetData(), (SIZE_T)Relay.Num(), kReliableChannel, ESendMode::Reliable);
            });
        }
        return true;
    }

    void NetSimulation::OnConnectionLeft(FNetWorldState& State, uint32 Connection)
    {
        State.SimPlayerOfConnection.Remove(Connection);
    }
}
