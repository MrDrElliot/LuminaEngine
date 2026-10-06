#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "Networking/INetworkTransport.h"
#include "Net/NetPrediction.h"

namespace Lumina
{
    // Wire-level players for a dedicated server that never load the map, so one process can stand in for hundreds.
    class FNetLoadBotSwarm
    {
    public:

        struct FConfig
        {
            uint32  Count          = 0;
            FString Host           = "127.0.0.1";
            uint16  Port           = 7777;
            float   JoinsPerSecond = 50.0f;
            float   CommandHz      = 60.0f;
            float   WanderRadius   = 400.0f;
            float   ChatSeconds    = 0.0f;
            float   AttackSeconds  = 0.0f;
            float   StatsSeconds   = 5.0f;
            FString RpcClass       = "NetStress.StressPlayer";
        };

        // Starts only when the process was given -netbots=N.
        static void StartFromCommandLine();
        static void Stop();
        static void Tick();

    private:

        struct FBot
        {
            TUniquePtr<INetworkTransport> Transport;
            FConnectionHandle             Server;
            TVector<FNetworkEvent>        Events;

            uint32  PeerId      = 0;
            uint32  PawnGuid    = 0;
            bool    bConnected  = false;
            bool    bReadySent  = false;
            bool    bLost       = false;
            double  JoinedAt    = 0.0;
            double  PawnAt      = 0.0;

            uint32  NextSeq     = 1;
            double  CommandDebt = 0.0;
            TVector<FNetMoveCommand> Recent;

            FVector3 Position   = FVector3(0.0f);
            FVector3 Home       = FVector3(0.0f);
            bool     bHasPosition = false;
            float    Yaw        = 0.0f;
            double   NextTurn   = 0.0;
            double   NextChat   = 0.0;
            double   NextAttack = 0.0;
            uint32   Rng        = 1;

            uint64  BytesIn     = 0;
            uint64  BytesOut    = 0;
            uint64  BytesInByType[16] = {};
            uint32  Snapshots   = 0;
            uint32  RpcsIn      = 0;
            double  LastSnapshot = 0.0;
            float   Travelled    = 0.0f;
            double  WorstSnapshotGap = 0.0;
        };

        void Begin(const FConfig& InConfig);
        void TickAll();
        void TickBot(FBot& Bot, double Now, double DeltaTime);
        void Receive(FBot& Bot, double Now, const uint8* Data, SIZE_T Size);
        void SendCommands(FBot& Bot, double DeltaTime);
        void SendRpc(FBot& Bot, uint32 RpcId, uint8 Target, uint8 Flags, const TVector<uint8>& Payload);
        void LogStats(double Now);

        FConfig        Config;
        TVector<FBot>  Bots;
        uint32         Launched   = 0;
        double         StartTime  = 0.0;
        double         LastTick   = 0.0;
        double         JoinDebt   = 0.0;
        double         LastStats  = 0.0;
        double         TickMsSum  = 0.0;
        double         TickMsMax  = 0.0;
        uint32         TickCount  = 0;
        uint32         ChatRpcId   = 0;
        uint32         AttackRpcId = 0;
        uint32         ProtocolHash = 0;
    };
}
