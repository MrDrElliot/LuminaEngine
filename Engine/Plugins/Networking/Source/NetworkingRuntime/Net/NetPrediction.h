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
        uint32                        OwningConnectionId = 0;
        uint32                        LastProcessed = 0;
        uint32                        SkippedMoves  = 0;
        float                         Budget        = 0.0f;
        float                         CatchUp       = 0.0f;     // budget the cap clipped, granted back a little per tick
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

        // How many unconfirmed moves ride in each upload, so a lost packet is covered by the next ones.
        constexpr uint32 MaxMovesPerUpload = 32;

        // One framed-ready MoveCommands message, shared by the predicting client and the load bots.
        NETWORKINGRUNTIME_API void WriteMoveCommands(TVector<uint8>& Buffer, uint32 Guid, const FNetMoveCommand* Commands, uint32 Count);

        // False on a truncated message or one carrying more than MaxMovesPerUpload moves.
        NETWORKINGRUNTIME_API bool ReadMoveCommands(const uint8* Data, SIZE_T Size, uint32& OutGuid, TVector<FNetMoveCommand>& OutCommands);

        // Quantized exactly as a client's own step records it, from a world-space move and a yaw in degrees.
        NETWORKINGRUNTIME_API FNetMoveCommand MakeMoveCommand(uint32 Seq, const FVector3& Move, float YawDegrees, uint32 Buttons);

        // The view pitch the host applies for a command, held to the range its replication can carry.
        NETWORKINGRUNTIME_API float PitchDegrees(int16 Pitch);

        // After roles refresh, decides who simulates each networked character.
        void RefreshDrives(CWorld* World, ECS::FRegistry& Registry, bool bServer);

        // Host only. A new owner starts its sequence at 1, so its server state restarts too; owner 0 removes it.
        NETWORKINGRUNTIME_API void RefreshServerPrediction(ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 OwningConnectionId);

        NETWORKINGRUNTIME_API float FixedStepOf(CWorld* World);

        // Physics calls this around every fixed character step. The client records its own moves here.
        NETWORKINGRUNTIME_API void OnCharacterStep(ECS::FRegistry& Registry, Physics::IPhysicsScene& Scene, bool bAfter);

        NETWORKINGRUNTIME_API void SendCommands(ECS::FRegistry& Registry, FNetWorldState& State);
        NETWORKINGRUNTIME_API void ReceiveCommands(ECS::FRegistry& Registry, Physics::IPhysicsScene& Scene, float FixedDt, uint32 Sender, const uint8* Data, SIZE_T Size);

        NETWORKINGRUNTIME_API void TickBudgets(ECS::FRegistry& Registry, float FixedDt, float DeltaTime);
        NETWORKINGRUNTIME_API void WriteMoveAck(TVector<uint8>& Buffer, uint32 Guid, uint32 Seq, const Physics::FCharacterNetState& State);
        NETWORKINGRUNTIME_API void SendAcks(ECS::FRegistry& Registry, FNetWorldState& State, float DeltaTime);
        NETWORKINGRUNTIME_API void ReceiveAck(ECS::FRegistry& Registry, FNetWorldState& State, Physics::IPhysicsScene& Scene, float FixedDt, const uint8* Data, SIZE_T Size);
    }
}
