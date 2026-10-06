#include "RuntimePCH.h"
#include "NetLoadBots.h"

#include "NetReplication.h"
#include "NetWorldState.h"
#include "NetworkGlobals.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Serialization/NetQuantize.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

#include <cstdlib>

namespace Lumina
{
    namespace
    {
        TUniquePtr<FNetLoadBotSwarm> GSwarm;

        // Several unconfirmed moves ride in each upload, so a lost packet is covered by the next one.
        constexpr uint32 MovesPerUpload = 3;

        float CommandLineFloat(const char* Name, float Default)
        {
            if (TOptional<FFixedString> Value = GCommandLine->Get(Name))
            {
                return std::strtof(Value->c_str(), nullptr);
            }
            return Default;
        }

        uint32 StableRpcId(const FString& Text)
        {
            uint32 Hash = 2166136261u;
            for (char Character : Text)
            {
                Hash ^= static_cast<uint8>(Character);
                Hash *= 16777619u;
            }
            return Hash;
        }

        uint32 NextRandom(uint32& State)
        {
            State ^= State << 13;
            State ^= State >> 17;
            State ^= State << 5;
            return State;
        }

        float RandomUnit(uint32& State)
        {
            return static_cast<float>(NextRandom(State) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
        }
    }

    void FNetLoadBotSwarm::StartFromCommandLine()
    {
        TOptional<int> Count = GCommandLine->GetInt("netbots");
        if (!Count.has_value() || Count.value() <= 0)
        {
            return;
        }

        FConfig Config;
        Config.Count = static_cast<uint32>(Count.value());
        if (TOptional<FFixedString> Connect = GCommandLine->Get("connect"))
        {
            const FString Address(Connect->c_str());
            const size_t Colon = Address.find_last_of(':');
            Config.Host = Colon == FString::npos ? Address : Address.substr(0, Colon);
            if (Colon != FString::npos)
            {
                Config.Port = static_cast<uint16>(std::strtoul(Address.c_str() + Colon + 1, nullptr, 10));
            }
        }
        Config.JoinsPerSecond = CommandLineFloat("botjoinrate", Config.JoinsPerSecond);
        Config.CommandHz      = CommandLineFloat("botcommandhz", Config.CommandHz);
        Config.WanderRadius   = CommandLineFloat("botradius", Config.WanderRadius);
        Config.ChatSeconds    = CommandLineFloat("botchat", Config.ChatSeconds);
        Config.AttackSeconds  = CommandLineFloat("botattack", Config.AttackSeconds);
        Config.StatsSeconds   = CommandLineFloat("netstats", Config.StatsSeconds);
        if (TOptional<FFixedString> RpcClass = GCommandLine->Get("botrpcclass"))
        {
            Config.RpcClass = FString(RpcClass->c_str());
        }

        GSwarm = MakeUnique<FNetLoadBotSwarm>();
        GSwarm->Begin(Config);
    }

    void FNetLoadBotSwarm::Stop()
    {
        GSwarm.reset();
    }

    void FNetLoadBotSwarm::Tick()
    {
        if (GSwarm != nullptr)
        {
            GSwarm->TickAll();
        }
    }

    void FNetLoadBotSwarm::Begin(const FConfig& InConfig)
    {
        Config       = InConfig;
        ChatRpcId    = StableRpcId(Config.RpcClass + ".Chat");
        AttackRpcId  = StableRpcId(Config.RpcClass + ".Attack");
        StartTime    = PlatformTime::Seconds();
        LastTick     = StartTime;
        LastStats    = StartTime;
        Bots.resize(Config.Count);
        LOG_DISPLAY("[NetBots] {} bots joining {}:{} at {:.0f} per second, wander radius {:.0f} m, chat every {:.1f} s, attack every {:.1f} s",
            Config.Count, Config.Host.c_str(), Config.Port, Config.JoinsPerSecond, Config.WanderRadius, Config.ChatSeconds, Config.AttackSeconds);
    }

    void FNetLoadBotSwarm::TickAll()
    {
        // The type table behind the hash is built once, so asking before every module registered would freeze it short.
        if (ProtocolHash == 0)
        {
            ProtocolHash = Net::GetProtocolHash();
        }

        const double Now = PlatformTime::Seconds();
        const double DeltaTime = Math::Min(Now - LastTick, 0.25);
        LastTick = Now;

        // Joins are paced, since a real launch never lands every player in the same frame.
        JoinDebt += DeltaTime * Config.JoinsPerSecond;
        while (JoinDebt >= 1.0 && Launched < Config.Count)
        {
            JoinDebt -= 1.0;
            FBot& Bot = Bots[Launched];
            Bot.Rng = 0x9E3779B9u ^ (Launched * 2654435761u + 1u);
            Bot.Yaw = RandomUnit(Bot.Rng) * 360.0f;
            Bot.Transport.reset(Network::CreateTransport());
            FConnectParams Params;
            Params.Address.Host = Config.Host;
            Params.Address.Port = Config.Port;
            Params.ChannelCount = 2;
            Bot.Server   = Bot.Transport->ConnectToServer(Params);
            Bot.JoinedAt = Now;
            ++Launched;
        }

        Task::ParallelFor(Launched, [this, Now, DeltaTime](uint32 Index)
        {
            TickBot(Bots[Index], Now, DeltaTime);
        });

        const double TickMs = (PlatformTime::Seconds() - Now) * 1000.0;
        TickMsSum += TickMs;
        TickMsMax = Math::Max(TickMsMax, TickMs);
        ++TickCount;

        if (Config.StatsSeconds > 0.0f && Now - LastStats >= Config.StatsSeconds)
        {
            LogStats(Now);
        }
    }

    void FNetLoadBotSwarm::TickBot(FBot& Bot, double Now, double DeltaTime)
    {
        if (Bot.Transport == nullptr || Bot.bLost)
        {
            return;
        }

        Bot.Events.clear();
        Bot.Transport->Service(Bot.Events);
        for (const FNetworkEvent& Event : Bot.Events)
        {
            switch (Event.Type)
            {
                case ENetworkEventType::Connected:
                    Bot.bConnected = true;
                    break;
                case ENetworkEventType::Disconnected:
                    Bot.bLost = true;
                    return;
                case ENetworkEventType::Data:
                    Bot.BytesIn += Event.Data.size();
                    Receive(Bot, Now, Event.Data.data(), Event.Data.size());
                    break;
            }
        }

        if (Bot.PawnGuid != 0)
        {
            SendCommands(Bot, DeltaTime);

            if (Config.ChatSeconds > 0.0f && Now >= Bot.NextChat)
            {
                Bot.NextChat = Now + Config.ChatSeconds * (0.5 + RandomUnit(Bot.Rng));
                static const char* const Lines[] = { "anyone near the bank?", "lfg dungeon run", "selling iron ore 20g each", "gg", "where is the quest giver" };
                const FString Text = Lines[NextRandom(Bot.Rng) % 5];
                TVector<uint8> Payload;
                FNetArchive Writer(Payload);
                WriteVarUInt(Writer, static_cast<uint32>(Text.size()));
                Writer.Serialize(const_cast<char*>(Text.data()), static_cast<int64>(Text.size()));
                SendRpc(Bot, ChatRpcId, static_cast<uint8>(ERpcTarget::Broadcast), static_cast<uint8>(ENetFlags::Unreliable), Payload);
            }

            if (Config.AttackSeconds > 0.0f && Now >= Bot.NextAttack)
            {
                Bot.NextAttack = Now + Config.AttackSeconds * (0.5 + RandomUnit(Bot.Rng));
                SendRpc(Bot, AttackRpcId, static_cast<uint8>(ERpcTarget::Host), static_cast<uint8>(ENetFlags::None), {});
            }
        }

        Bot.Transport->Flush();
    }

    void FNetLoadBotSwarm::Receive(FBot& Bot, double Now, const uint8* Data, SIZE_T Size)
    {
        Net::ForEachFramedMessage(Data, Size, [&](const uint8* Message, SIZE_T MessageSize)
        {
            if (MessageSize == 0)
            {
                return;
            }
            const uint8 Type = Message[0];
            Bot.BytesInByType[Type < 16 ? Type : 0] += MessageSize + 2;

            FNetArchive Reader(Message, MessageSize);
            uint8 TypeByte = 0;
            Reader << TypeByte;

            switch (static_cast<ENetMessage>(Type))
            {
                case ENetMessage::AssignPeerId:
                    Reader << Bot.PeerId;
                    break;

                case ENetMessage::Welcome:
                {
                    uint32 Protocol = 0;
                    Reader << Protocol;
                    if (Protocol != ProtocolHash)
                    {
                        LOG_ERROR("[NetBots] Server protocol {:#x} does not match this build's {:#x}.", Protocol, ProtocolHash);
                        Bot.bLost = true;
                        return;
                    }
                    if (!Bot.bReadySent)
                    {
                        Bot.bReadySent = true;
                        TVector<uint8> Ready;
                        FNetArchive Writer(Ready);
                        uint8 ReadyType = static_cast<uint8>(ENetMessage::ClientReady);
                        uint32 ReadyProtocol = ProtocolHash;
                        Writer << ReadyType;
                        Writer << ReadyProtocol;
                        Net::SendFramed(*Bot.Transport, Bot.Server, Ready.data(), static_cast<SIZE_T>(Ready.size()), 0, ESendMode::Reliable);
                        Bot.BytesOut += Ready.size() + 2;
                    }
                    break;
                }

                case ENetMessage::OwnershipUpdate:
                {
                    uint16 Count = 0;
                    Reader << Count;
                    for (uint16 Index = 0; Index < Count && !Reader.HasError(); ++Index)
                    {
                        const uint32 Guid  = Net::ReadNetGuid(Reader);
                        const uint32 Owner = ReadVarUInt(Reader);
                        if (Owner == Bot.PeerId && Bot.PeerId != 0 && Bot.PawnGuid == 0)
                        {
                            Bot.PawnGuid = Guid;
                            Bot.PawnAt   = Now;
                            Bot.NextChat   = Now + Config.ChatSeconds * RandomUnit(Bot.Rng);
                            Bot.NextAttack = Now + Config.AttackSeconds * RandomUnit(Bot.Rng);
                        }
                    }
                    break;
                }

                case ENetMessage::MoveAck:
                {
                    (void)Net::ReadNetGuid(Reader);
                    (void)ReadVarUInt(Reader);
                    NetQuantize::FQuantizedVector Position;
                    Position.Read(Reader);
                    if (!Reader.HasError())
                    {
                        const FVector3 Acked = Position.ToVector();
                        if (!Bot.bHasPosition)
                        {
                            Bot.Home = Acked;
                        }
                        else
                        {
                            Bot.Travelled += Math::Length(FVector2(Acked.x - Bot.Position.x, Acked.z - Bot.Position.z));
                        }
                        Bot.Position     = Acked;
                        Bot.bHasPosition = true;
                    }
                    break;
                }

                case ENetMessage::TransformSnapshot:
                    if (Bot.LastSnapshot > 0.0)
                    {
                        Bot.WorstSnapshotGap = Math::Max(Bot.WorstSnapshotGap, Now - Bot.LastSnapshot);
                    }
                    Bot.LastSnapshot = Now;
                    ++Bot.Snapshots;
                    break;

                case ENetMessage::ScriptRpc:
                    ++Bot.RpcsIn;
                    break;

                default:
                    break;
            }
        });
    }

    void FNetLoadBotSwarm::SendCommands(FBot& Bot, double DeltaTime)
    {
        // Commands are owed at the server's fixed step, since the host only simulates the moves it is sent.
        Bot.CommandDebt += DeltaTime * Config.CommandHz;
        const uint32 Owed = static_cast<uint32>(Bot.CommandDebt);
        if (Owed == 0)
        {
            return;
        }
        Bot.CommandDebt -= Owed;

        const double Now = LastTick;
        if (Now >= Bot.NextTurn)
        {
            Bot.NextTurn = Now + 2.0 + RandomUnit(Bot.Rng) * 4.0;
            Bot.Yaw += (RandomUnit(Bot.Rng) - 0.5f) * 140.0f;
        }
        // Players linger around where they arrived, the way a hub or a quest area holds them.
        const FVector2 FromHome(Bot.Position.x - Bot.Home.x, Bot.Position.z - Bot.Home.z);
        if (Bot.bHasPosition && Math::Length(FromHome) > Config.WanderRadius)
        {
            Bot.Yaw = Math::Degrees(std::atan2(-FromHome.x, -FromHome.y));
        }

        const float YawRadians = Math::Radians(Bot.Yaw);
        const FVector3 Move(std::sin(YawRadians), 0.0f, std::cos(YawRadians));
        for (uint32 Step = 0; Step < Math::Min(Owed, 8u); ++Step)
        {
            if (Bot.Recent.size() >= MovesPerUpload)
            {
                Bot.Recent.erase(Bot.Recent.begin());
            }
            Bot.Recent.push_back(NetPrediction::MakeMoveCommand(Bot.NextSeq++, Move, Bot.Yaw, 0));
        }

        TVector<uint8> Upload;
        NetPrediction::WriteMoveCommands(Upload, Bot.PawnGuid, Bot.Recent.data(), static_cast<uint32>(Bot.Recent.size()));
        Net::SendFramed(*Bot.Transport, Bot.Server, Upload.data(), static_cast<SIZE_T>(Upload.size()), 1, ESendMode::UnreliableSequenced);
        Bot.BytesOut += Upload.size() + 2;
    }

    void FNetLoadBotSwarm::SendRpc(FBot& Bot, uint32 RpcId, uint8 Target, uint8 Flags, const TVector<uint8>& Payload)
    {
        TVector<uint8> Batch;
        Net::AppendScriptRpc(Batch, Bot.PawnGuid, 0, RpcId, static_cast<ERpcTarget>(Target), Flags, Bot.PeerId,
            Payload.data(), static_cast<uint32>(Payload.size()));
        const bool bReliable = (Flags & static_cast<uint8>(ENetFlags::Unreliable)) == 0;
        Bot.Transport->Send(Bot.Server, Batch.data(), static_cast<SIZE_T>(Batch.size()), bReliable ? 0 : 1,
            bReliable ? ESendMode::Reliable : ESendMode::UnreliableSequenced);
        Bot.BytesOut += Batch.size();
    }

    void FNetLoadBotSwarm::LogStats(double Now)
    {
        const double Seconds = Math::Max(Now - LastStats, 0.001);
        LastStats = Now;

        uint32 Connected = 0, Pawned = 0, Lost = 0;
        uint64 InTotal = 0, OutTotal = 0, InMax = 0;
        uint64 ByType[16] = {};
        uint32 SnapshotMin = UINT32_MAX, SnapshotSum = 0, RpcsIn = 0, RttMax = 0, RttSum = 0, RttCount = 0;
        double WorstGap = 0.0, JoinSum = 0.0, TravelSum = 0.0;
        uint32 JoinCount = 0;
        TVector<FConnectionStats> Peer;

        for (FBot& Bot : Bots)
        {
            if (Bot.Transport == nullptr)
            {
                continue;
            }
            Lost += Bot.bLost ? 1 : 0;
            Connected += (Bot.bConnected && !Bot.bLost) ? 1 : 0;
            if (Bot.PawnGuid != 0 && !Bot.bLost)
            {
                ++Pawned;
                SnapshotMin = Math::Min(SnapshotMin, Bot.Snapshots);
                SnapshotSum += Bot.Snapshots;
                WorstGap = Math::Max(WorstGap, Bot.WorstSnapshotGap);
                TravelSum += Bot.Travelled;
                if (Bot.PawnAt > 0.0)
                {
                    JoinSum += Bot.PawnAt - Bot.JoinedAt;
                    ++JoinCount;
                }
            }
            InTotal  += Bot.BytesIn;
            OutTotal += Bot.BytesOut;
            InMax     = Math::Max(InMax, Bot.BytesIn);
            RpcsIn   += Bot.RpcsIn;
            for (uint32 Type = 0; Type < 16; ++Type)
            {
                ByType[Type] += Bot.BytesInByType[Type];
                Bot.BytesInByType[Type] = 0;
            }

            Peer.clear();
            Bot.Transport->GetConnectionStats(Peer);
            for (const FConnectionStats& Stats : Peer)
            {
                RttMax = Math::Max(RttMax, Stats.RoundTripTimeMs);
                RttSum += Stats.RoundTripTimeMs;
                ++RttCount;
            }

            Bot.BytesIn = Bot.BytesOut = 0;
            Bot.Snapshots = Bot.RpcsIn = 0;
            Bot.WorstSnapshotGap = 0.0;
            Bot.Travelled = 0.0f;
        }

        const double PawnedDiv = Math::Max(Pawned, 1u);
        LOG_DISPLAY("[NetBots][Stats] launched {} connected {} playing {} lost {}  in {:.1f} KB/s (per bot avg {:.2f} max {:.2f})  out {:.1f} KB/s  rtt avg {} max {} ms  snapshots/s avg {:.1f} min {:.1f}  worst gap {:.0f} ms  rpcs in/s {:.0f}  join {:.2f} s  tick avg {:.2f} max {:.2f} ms  speed {:.2f} m/s",
            Launched, Connected, Pawned, Lost,
            InTotal / 1024.0 / Seconds, InTotal / 1024.0 / Seconds / PawnedDiv, InMax / 1024.0 / Seconds,
            OutTotal / 1024.0 / Seconds,
            RttCount ? RttSum / RttCount : 0u, RttMax,
            SnapshotSum / Seconds / PawnedDiv, (SnapshotMin == UINT32_MAX ? 0u : SnapshotMin) / Seconds,
            WorstGap * 1000.0, RpcsIn / Seconds, JoinCount ? JoinSum / JoinCount : 0.0,
            TickCount ? TickMsSum / TickCount : 0.0, TickMsMax, TravelSum / Seconds / PawnedDiv);

        static const char* const Names[16] = { "other", "transform", "rpc", "peer", "ownership", "spawn", "despawn", "property",
                                                "clienttransform", "welcome", "ready", "objects", "assets", "names", "commands", "moveack" };
        FString Breakdown;
        for (uint32 Type = 0; Type < 16; ++Type)
        {
            if (ByType[Type] > 0)
            {
                Breakdown += Format("  {} {:.1f}", Names[Type], ByType[Type] / 1024.0 / Seconds).c_str();
            }
        }
        LOG_DISPLAY("[NetBots][Stats] in KB/s by type{}", Breakdown.c_str());

        TickMsSum = 0.0;
        TickMsMax = 0.0;
        TickCount = 0;
    }
}
