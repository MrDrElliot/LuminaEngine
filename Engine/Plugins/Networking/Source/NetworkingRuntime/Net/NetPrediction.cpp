#include "RuntimePCH.h"
#include "NetPrediction.h"
#include "NetWorldState.h"
#include "NetReplication.h"
#include "Components/NetworkComponent.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Serialization/NetQuantize.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "Networking/INetworkTransport.h"
#include "Log/Log.h"
#include "Config/NetworkSettings.h"

namespace Lumina::NetPrediction
{
    namespace
    {
        // A client may run this far ahead of real time before its extra moves wait for the next frame.
        constexpr float MaxBudgetSeconds = 0.25f;

        // Older unconfirmed moves than this are dropped, which only happens when the host stops answering.
        constexpr uint32 MaxPendingMoves = 240;

        // How many unconfirmed moves ride in each upload, so a lost packet is covered by the next ones.
        constexpr uint32 MaxMovesPerUpload = 32;

        // Below these the client agrees with the host, since the wire carries millimeters and centimeters per second.
        constexpr float PositionTolerance = 0.02f;
        constexpr float VelocityTolerance = 0.1f;

        enum ECommandBits : uint8
        {
            Cmd_Jump        = 1 << 0,
            Cmd_SameMove    = 1 << 1,
            Cmd_SameLook    = 1 << 2,
            Cmd_SameButtons = 1 << 3,
        };

        uint32 ZigZag(int32 Value)
        {
            return ((uint32)Value << 1) ^ (uint32)(Value >> 31);
        }

        int32 UnZigZag(uint32 Value)
        {
            return (int32)(Value >> 1) ^ -(int32)(Value & 1);
        }

        float FixedStepOf(CWorld* World)
        {
            return 1.0f / Math::Max(10.0f, World->GetDefaultWorldSettings().PhysicsHz);
        }

        int8 QuantizeAxis(float Value)
        {
            return (int8)Math::Clamp((int32)Math::Round(Math::Clamp(Value, -1.0f, 1.0f) * 127.0f), -127, 127);
        }

        uint16 QuantizeYaw(float Degrees)
        {
            float Wrapped = std::fmod(Degrees, 360.0f);
            if (Wrapped < 0.0f)
            {
                Wrapped += 360.0f;
            }
            return (uint16)((uint32)Math::Round(Wrapped * (65536.0f / 360.0f)) & 0xFFFF);
        }

        float YawDegrees(uint16 Yaw)
        {
            return (float)Yaw * (360.0f / 65536.0f);
        }

        Physics::FCharacterMoveInput ToInput(const FNetMoveCommand& Command, float MoveSpeed)
        {
            Physics::FCharacterMoveInput Input;
            const FVector3 Move((float)Command.MoveX / 127.0f, 0.0f, (float)Command.MoveZ / 127.0f);
            const float Magnitude = Math::Length(Move);
            if (Magnitude > 1.0e-3f)
            {
                Input.Direction = Move / Magnitude;
                Input.Throttle  = Math::Min(Magnitude, 1.0f);
                Input.bHasMove  = true;
            }
            Input.LookYaw   = YawDegrees(Command.Yaw);
            Input.MoveSpeed = MoveSpeed;
            Input.Buttons   = Command.Buttons;
            Input.bJump     = Command.bJump;
            return Input;
        }

        bool IsCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            return Registry.HasAll<SCharacterPhysicsComponent, SCharacterMovementComponent, SCharacterControllerComponent>(Entity);
        }

        void WriteState(FNetArchive& Writer, const Physics::FCharacterNetState& State)
        {
            NetQuantize::FQuantizedVector::FromVector(State.Position).Write(Writer);
            WriteVarUInt(Writer, ZigZag((int32)Math::Round(State.Velocity.x * 100.0f)));
            WriteVarUInt(Writer, ZigZag((int32)Math::Round(State.Velocity.y * 100.0f)));
            WriteVarUInt(Writer, ZigZag((int32)Math::Round(State.Velocity.z * 100.0f)));
            uint8 Grounded = State.bGrounded ? 1 : 0;
            Writer << Grounded;
            WriteVarUInt(Writer, (uint32)Math::Max(State.JumpCount, 0));
        }

        void ReadState(FNetArchive& Reader, Physics::FCharacterNetState& State)
        {
            NetQuantize::FQuantizedVector Position;
            Position.Read(Reader);
            State.Position = Position.ToVector();
            State.Velocity.x = (float)UnZigZag(ReadVarUInt(Reader)) / 100.0f;
            State.Velocity.y = (float)UnZigZag(ReadVarUInt(Reader)) / 100.0f;
            State.Velocity.z = (float)UnZigZag(ReadVarUInt(Reader)) / 100.0f;
            uint8 Grounded = 0;
            Reader << Grounded;
            State.bGrounded = Grounded != 0;
            State.JumpCount = (int32)ReadVarUInt(Reader);
        }
    }

    bool IsPredictedCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return IsCharacter(Registry, Entity);
    }

    FNetMoveCommand MakeMoveCommand(uint32 Seq, const FVector3& Move, float YawDegrees, uint32 Buttons)
    {
        FNetMoveCommand Command;
        Command.Seq     = Seq;
        Command.MoveX   = QuantizeAxis(Move.x);
        Command.MoveZ   = QuantizeAxis(Move.z);
        Command.Yaw     = QuantizeYaw(YawDegrees);
        Command.Buttons = Buttons;
        return Command;
    }

    void RefreshDrives(CWorld* World, ECS::FRegistry& Registry, bool bServer)
    {
        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        if (Scene == nullptr)
        {
            return;
        }

        TVector<ECS::FEntity> Characters;
        for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
        {
            if (IsCharacter(Registry, Entity))
            {
                Characters.push_back(Entity);
            }
        }

        for (ECS::FEntity Entity : Characters)
        {
            const SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
            Physics::ECharacterNetDrive Drive;
            if (bServer)
            {
                Drive = Net.OwningConnectionId != 0 ? Physics::ECharacterNetDrive::Commands : Physics::ECharacterNetDrive::Simulated;
            }
            else
            {
                Drive = Net.LocalRole == ENetRole::AutonomousProxy ? Physics::ECharacterNetDrive::Simulated : Physics::ECharacterNetDrive::FollowTransform;
            }

            FNetCharacterDrive& Applied = Registry.GetOrEmplace<FNetCharacterDrive>(Entity);
            if (Applied.Drive == (uint8)Drive)
            {
                continue;
            }

            // The capsule may not exist on the frame the entity arrived, so the drive is retried until it does.
            Physics::FCharacterNetState Probe;
            if (!Scene->GetCharacterNetState(Entity, Probe))
            {
                continue;
            }
            Scene->SetCharacterNetDrive(Entity, Drive);
            Applied.Drive = (uint8)Drive;

            const bool bPredictsHere = !bServer && Net.LocalRole == ENetRole::AutonomousProxy;
            const bool bServesCommands = bServer && Net.OwningConnectionId != 0;
            if (bPredictsHere)
            {
                Registry.EmplaceOrReplace<FNetPredictionClient>(Entity);
            }
            else
            {
                Registry.Remove<FNetPredictionClient>(Entity);
            }
            if (bServesCommands)
            {
                Registry.EmplaceOrReplace<FNetPredictionServer>(Entity).Budget = 4.0f;
            }
            else
            {
                Registry.Remove<FNetPredictionServer>(Entity);
            }
        }
    }

    void OnCharacterStep(CWorld* World, float FixedDt, bool bAfter)
    {
        if (World == nullptr || World->GetNetMode() != ENetMode::Client)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        auto View = Registry.View<FNetPredictionClient>();
        for (ECS::FEntity Entity : View)
        {
            FNetPredictionClient& Prediction = Registry.Get<FNetPredictionClient>(Entity);
            SCharacterMovementComponent* Movement = Registry.TryGet<SCharacterMovementComponent>(Entity);
            SCharacterControllerComponent* Controller = Registry.TryGet<SCharacterControllerComponent>(Entity);
            if (Movement == nullptr || Controller == nullptr)
            {
                continue;
            }

            if (bAfter)
            {
                if (!Prediction.Pending.empty() && !Prediction.Pending.back().bHasAfter)
                {
                    Prediction.Pending.back().bHasAfter = Scene->GetCharacterNetState(Entity, Prediction.Pending.back().After);
                }
                continue;
            }

            FNetMoveCommand Command;
            Command.Seq = Prediction.NextSeq++;
            const FVector3 Move = Movement->bHasPendingMoveInput
                ? Movement->PendingMoveDirection * Movement->PendingMoveThrottle : FVector3(0.0f);
            Command.MoveX   = QuantizeAxis(Move.x);
            Command.MoveZ   = QuantizeAxis(Move.z);
            Command.Yaw     = QuantizeYaw(Movement->PendingLookYaw);
            Command.Pitch   = (int16)Math::Clamp((int32)Math::Round(Controller->LookInput.y * 100.0f), -32767, 32767);
            Command.Buttons = Controller->Buttons;
            Command.bJump   = Movement->bPendingJump;

            // This peer steps on what the host will receive, so the two runs agree to the bit.
            const Physics::FCharacterMoveInput Input = ToInput(Command, Movement->MoveSpeed);
            Movement->PendingMoveDirection = Input.Direction;
            Movement->PendingMoveThrottle  = Input.Throttle;
            Movement->bHasPendingMoveInput = Input.bHasMove;
            Movement->PendingLookYaw       = Input.LookYaw;

            if (Prediction.Pending.size() >= MaxPendingMoves)
            {
                Prediction.Pending.erase(Prediction.Pending.begin());
            }
            Prediction.Pending.push_back(FPredictedMove{ Command, Movement->MoveSpeed, {}, false });
        }
    }

    void WriteMoveCommands(TVector<uint8>& Buffer, uint32 Guid, const FNetMoveCommand* Commands, uint32 Count)
    {
        FNetArchive Writer(Buffer);
        uint8 Type = static_cast<uint8>(ENetMessage::MoveCommands);
        Writer << Type;
        Net::WriteNetGuid(Writer, Guid);
        WriteVarUInt(Writer, Count > 0 ? Commands[0].Seq : 0);
        WriteVarUInt(Writer, Count);

        // Held input repeats every step, so each field is only written when it differs from the move before.
        const FNetMoveCommand* Previous = nullptr;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            const FNetMoveCommand& Command = Commands[Index];
            const bool bSameMove    = Previous && Previous->MoveX == Command.MoveX && Previous->MoveZ == Command.MoveZ;
            const bool bSameLook    = Previous && Previous->Yaw == Command.Yaw && Previous->Pitch == Command.Pitch;
            const bool bSameButtons = Previous && Previous->Buttons == Command.Buttons;
            uint8 Bits = (Command.bJump ? Cmd_Jump : 0) | (bSameMove ? Cmd_SameMove : 0)
                       | (bSameLook ? Cmd_SameLook : 0) | (bSameButtons ? Cmd_SameButtons : 0);
            Writer << Bits;
            if (!bSameMove)
            {
                int8 X = Command.MoveX, Z = Command.MoveZ;
                Writer << X;
                Writer << Z;
            }
            if (!bSameLook)
            {
                uint16 Yaw = Command.Yaw;
                int16 Pitch = Command.Pitch;
                Writer << Yaw;
                Writer << Pitch;
            }
            if (!bSameButtons)
            {
                WriteVarUInt(Writer, Command.Buttons);
            }
            Previous = &Command;
        }
    }

    void SendCommands(ECS::FRegistry& Registry, FNetWorldState& State)
    {
        auto View = Registry.View<FNetPredictionClient, SNetworkComponent>();
        for (ECS::FEntity Entity : View)
        {
            const FNetPredictionClient& Prediction = Registry.Get<FNetPredictionClient>(Entity);
            const SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
            if (Prediction.Pending.empty() || Net.NetGUID.Value == 0)
            {
                continue;
            }

            const uint32 Count = Math::Min((uint32)Prediction.Pending.size(), MaxMovesPerUpload);

            const uint32 First = (uint32)Prediction.Pending.size() - Count;

            static thread_local TVector<FNetMoveCommand> Upload;
            Upload.clear();
            for (uint32 Index = First; Index < (uint32)Prediction.Pending.size(); ++Index)
            {
                Upload.push_back(Prediction.Pending[Index].Command);
            }
            TVector<uint8> Buffer;
            WriteMoveCommands(Buffer, Net.NetGUID.Value, Upload.data(), Count);
            Net::SendFramed(*State.Transport, State.ServerConnection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 1, ESendMode::UnreliableSequenced);
        }
    }

    void ReceiveCommands(CWorld* World, ECS::FRegistry& Registry, uint32 Sender, const uint8* Data, SIZE_T Size)
    {
        FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        if (State == nullptr || Scene == nullptr)
        {
            return;
        }

        FNetArchive Reader(Data, Size);
        uint8 Type = 0;
        Reader << Type;
        const uint32 Guid     = Net::ReadNetGuid(Reader);
        const uint32 FirstSeq = ReadVarUInt(Reader);
        const uint32 Count    = ReadVarUInt(Reader);
        if (Reader.HasError() || Count > MaxMovesPerUpload)
        {
            return;
        }

        const ECS::FEntity Entity = State->GuidTable.Find(FNetGUID{ Guid });
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return;
        }
        const SNetworkComponent* Net = Registry.TryGet<SNetworkComponent>(Entity);
        FNetPredictionServer* Prediction = Registry.TryGet<FNetPredictionServer>(Entity);
        SCharacterControllerComponent* Controller = Registry.TryGet<SCharacterControllerComponent>(Entity);
        if (Net == nullptr || Net->OwningConnectionId != Sender || Prediction == nullptr || Controller == nullptr)
        {
            return;
        }

        const float FixedDt = FixedStepOf(World);
        FNetMoveCommand Command;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            uint8 Bits = 0;
            Reader << Bits;
            if (!(Bits & Cmd_SameMove))
            {
                Reader << Command.MoveX;
                Reader << Command.MoveZ;
            }
            if (!(Bits & Cmd_SameLook))
            {
                Reader << Command.Yaw;
                Reader << Command.Pitch;
            }
            if (!(Bits & Cmd_SameButtons))
            {
                Command.Buttons = ReadVarUInt(Reader);
            }
            Command.bJump = (Bits & Cmd_Jump) != 0;
            Command.Seq   = FirstSeq + Index;
            if (Reader.HasError())
            {
                return;
            }

            if (Command.Seq <= Prediction->LastProcessed)
            {
                continue;
            }

            // Moves past the budget wait, since the client resends everything it has not seen confirmed.
            if (Prediction->Budget < 1.0f)
            {
                break;
            }
            Prediction->Budget -= 1.0f;

            Controller->LookInput.x = YawDegrees(Command.Yaw);
            Controller->LookInput.y = (float)Command.Pitch / 100.0f;
            Controller->Buttons     = Command.Buttons;

            Scene->SimulateCharacterStep(Entity, ToInput(Command, 0.0f), FixedDt, false);
            Prediction->LastProcessed = Command.Seq;
            Prediction->AckSeq        = Command.Seq;
            Prediction->bAckPending   = Scene->GetCharacterNetState(Entity, Prediction->AckState);
        }
    }

    void TickBudgets(CWorld* World, ECS::FRegistry& Registry, float DeltaTime)
    {
        const float FixedDt = FixedStepOf(World);
        const float Cap = MaxBudgetSeconds / FixedDt + 4.0f;
        for (ECS::FEntity Entity : Registry.View<FNetPredictionServer>())
        {
            FNetPredictionServer& Prediction = Registry.Get<FNetPredictionServer>(Entity);
            Prediction.Budget = Math::Min(Prediction.Budget + DeltaTime / FixedDt, Cap);
        }
    }

    void SendAcks(ECS::FRegistry& Registry, FNetWorldState& State, float DeltaTime)
    {
        const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
        const float AckInterval = 1.0f / Math::Max(Settings != nullptr ? Settings->MoveAckRate : 20.0f, 1.0f);

        auto View = Registry.View<FNetPredictionServer, SNetworkComponent>();
        for (ECS::FEntity Entity : View)
        {
            FNetPredictionServer& Prediction = Registry.Get<FNetPredictionServer>(Entity);
            const SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
            Prediction.TimeSinceAck += DeltaTime;
            if (!Prediction.bAckPending || Prediction.TimeSinceAck < AckInterval || Net.OwningConnectionId == 0 || Net.NetGUID.Value == 0)
            {
                continue;
            }
            Prediction.bAckPending  = false;
            Prediction.TimeSinceAck = 0.0f;

            TVector<uint8> Buffer;
            FNetArchive Writer(Buffer);
            uint8 Type = static_cast<uint8>(ENetMessage::MoveAck);
            Writer << Type;
            Net::WriteNetGuid(Writer, Net.NetGUID.Value);
            WriteVarUInt(Writer, Prediction.AckSeq);
            WriteState(Writer, Prediction.AckState);
            Net::SendFramed(*State.Transport, FConnectionHandle{ Net.OwningConnectionId }, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 1, ESendMode::UnreliableSequenced);
        }
    }

    void ReceiveAck(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
    {
        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        if (Scene == nullptr)
        {
            return;
        }

        FNetArchive Reader(Data, Size);
        uint8 Type = 0;
        Reader << Type;
        const uint32 Guid = Net::ReadNetGuid(Reader);
        const uint32 Seq  = ReadVarUInt(Reader);
        Physics::FCharacterNetState Host;
        ReadState(Reader, Host);
        if (Reader.HasError())
        {
            return;
        }

        const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
        FNetPredictionClient* Prediction = (Entity != ECS::NullEntity && Registry.IsValid(Entity))
            ? Registry.TryGet<FNetPredictionClient>(Entity) : nullptr;
        if (Prediction == nullptr || Seq <= Prediction->LastAcked)
        {
            return;
        }

        int32 Index = INDEX_NONE;
        for (int32 Candidate = 0; Candidate < (int32)Prediction->Pending.size(); ++Candidate)
        {
            if (Prediction->Pending[Candidate].Command.Seq == Seq)
            {
                Index = Candidate;
                break;
            }
        }
        Prediction->LastAcked = Seq;
        if (Index == INDEX_NONE || !Prediction->Pending[Index].bHasAfter)
        {
            return;
        }

        const Physics::FCharacterNetState& Predicted = Prediction->Pending[Index].After;
        const bool bAgrees = Math::Length(Predicted.Position - Host.Position) <= PositionTolerance
            && Math::Length(Predicted.Velocity - Host.Velocity) <= VelocityTolerance
            && Predicted.bGrounded == Host.bGrounded
            && Predicted.JumpCount == Host.JumpCount;

        if (!bAgrees)
        {
            // Rewind to what the host computed, then replay every move it has not seen yet on top of it.
            Physics::FCharacterNetState Corrected = Predicted;
            Corrected.Position  = Host.Position;
            Corrected.Velocity  = Host.Velocity;
            Corrected.bGrounded = Host.bGrounded;
            Corrected.JumpCount = Host.JumpCount;
            Scene->SetCharacterNetState(Entity, Corrected);

            const float FixedDt = FixedStepOf(World);
            for (int32 Later = Index + 1; Later < (int32)Prediction->Pending.size(); ++Later)
            {
                FPredictedMove& Move = Prediction->Pending[Later];
                Scene->SimulateCharacterStep(Entity, ToInput(Move.Command, Move.MoveSpeed), FixedDt, true);
                Move.bHasAfter = Scene->GetCharacterNetState(Entity, Move.After);
            }

            if (++Prediction->Corrections <= 5 || (Prediction->Corrections % 100) == 0)
            {
                LOG_DISPLAY("[Net][Predict] Correction {} on move {}, off by {:.3f} m, velocity {:.3f} m/s, grounded {}/{}, jumps {}/{}",
                    Prediction->Corrections, Seq, Math::Length(Predicted.Position - Host.Position),
                    Math::Length(Predicted.Velocity - Host.Velocity), Predicted.bGrounded, Host.bGrounded,
                    Predicted.JumpCount, Host.JumpCount);
            }
        }

        Prediction->Pending.erase(Prediction->Pending.begin(), Prediction->Pending.begin() + Index + 1);
    }
}
