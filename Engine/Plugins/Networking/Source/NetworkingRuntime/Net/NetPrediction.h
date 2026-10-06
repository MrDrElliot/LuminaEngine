#pragma once

#include "World/ECS/Registry.h"
#include "Containers/Vector.h"
#include "Physics/PhysicsScene.h"

namespace Lumina
{
    class CWorld;
    struct FNetWorldState;

    // One fixed step of a client's input, quantized before it is simulated so both peers run identical values.
    struct FNetMoveCommand
    {
        uint32 Seq     = 0;
        int8   MoveX   = 0;     // world-space direction times throttle, in 127ths
        int8   MoveZ   = 0;
        uint16 Yaw     = 0;     // 65536ths of a turn
        int16  Pitch   = 0;     // hundredths of a degree
        uint32 Buttons = 0;
        bool   bJump   = false;
    };

    struct FPredictedMove
    {
        FNetMoveCommand               Command;
        float                         MoveSpeed = 0.0f;
        Physics::FCharacterNetState   After;
        bool                          bHasAfter = false;
    };

    // Client, on the character this peer owns. Moves stay here until the host confirms them.
    struct FNetPredictionClient
    {
        TVector<FPredictedMove> Pending;
        uint32                  NextSeq    = 1;
        uint32                  LastAcked  = 0;
        uint32                  Corrections = 0;
    };

    // Host, on a character a client owns. Budget caps how many steps a client may claim per second of real time.
    struct FNetPredictionServer
    {
        uint32                        LastProcessed = 0;
        float                         Budget        = 0.0f;
        Physics::FCharacterNetState   AckState;
        uint32                        AckSeq        = 0;
        float                         TimeSinceAck  = 0.0f;
        bool                          bAckPending   = false;
    };

    // Which ECharacterNetDrive was last applied, so it is only pushed to physics when it changes.
    struct FNetCharacterDrive
    {
        uint8 Drive = 0xFF;
    };

    namespace NetPrediction
    {
        // A character moves by commands the host simulates, never by a pose its owner uploads.
        bool IsPredictedCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // One framed-ready MoveCommands message, shared by the predicting client and the load bots.
        void WriteMoveCommands(TVector<uint8>& Buffer, uint32 Guid, const FNetMoveCommand* Commands, uint32 Count);

        // Quantized exactly as a client's own step records it, from a world-space move and a yaw in degrees.
        FNetMoveCommand MakeMoveCommand(uint32 Seq, const FVector3& Move, float YawDegrees, uint32 Buttons);

        // After roles refresh, decides who simulates each networked character.
        void RefreshDrives(CWorld* World, ECS::FRegistry& Registry, bool bServer);

        // Physics calls this around every fixed character step. The client records its own moves here.
        void OnCharacterStep(CWorld* World, float FixedDt, bool bAfter);

        void SendCommands(ECS::FRegistry& Registry, FNetWorldState& State);
        void ReceiveCommands(CWorld* World, ECS::FRegistry& Registry, uint32 Sender, const uint8* Data, SIZE_T Size);

        void TickBudgets(CWorld* World, ECS::FRegistry& Registry, float DeltaTime);
        void SendAcks(ECS::FRegistry& Registry, FNetWorldState& State, float DeltaTime);
        void ReceiveAck(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size);
    }
}
