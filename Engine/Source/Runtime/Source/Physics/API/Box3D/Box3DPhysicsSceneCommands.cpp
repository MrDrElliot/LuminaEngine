#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "World/World.h"

namespace Lumina::Physics
{
    EPhysicsBodyStatus FBox3DPhysicsScene::GetBodyStatus(ECS::FEntity Entity) const
    {
        const FBodyRecord* Record = FindBodyRecord(Entity);
        return Record != nullptr ? Record->Status : EPhysicsBodyStatus::Missing;
    }

    const FBox3DPhysicsScene::FBodyRecord* FBox3DPhysicsScene::FindBodyRecord(ECS::FEntity Entity) const
    {
        const FBodyRecord* Record = RigidBodies.Find(Entity);
        return Record != nullptr ? Record : CharacterBodies.Find(Entity);
    }

    FPhysicsBodyTarget FBox3DPhysicsScene::MakeBodyTarget(uint32 Handle) const
    {
        FPhysicsBodyTarget Target;
        if (Handle < BodyHandles.size() && b3Body_IsValid(BodyHandles[Handle]))
        {
            Target.Scene = SceneIdentity;
            Target.Slot = Handle;
            Target.Generation = BodyGenerations[Handle];
        }
        return Target;
    }

    b3BodyId FBox3DPhysicsScene::ResolveTarget(const FPhysicsBodyTarget& Target) const
    {
        if (Target.Scene != SceneIdentity || Target.Slot >= BodyHandles.size()
            || BodyGenerations[Target.Slot] != Target.Generation)
        {
            return b3_nullBodyId;
        }
        return BodyHandles[Target.Slot];
    }

    bool FBox3DPhysicsScene::ReadBodyState(b3BodyId Body, FPhysicsBodyState& Out) const
    {
        Out = {};
        if (!b3Body_IsValid(Body))
        {
            return false;
        }
        Out.Position = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Body));
        Out.Rotation = Box3DUtils::FromB3Quat(b3Body_GetRotation(Body));
        Out.LinearVelocity = Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(Body));
        Out.AngularVelocity = Box3DUtils::FromB3Vec3(b3Body_GetAngularVelocity(Body));
        Out.CenterOfMass = Box3DUtils::FromB3Vec3(b3Body_GetWorldCenter(Body));
        Out.Mass = b3Body_GetMass(Body);
        Out.bAwake = b3Body_IsAwake(Body);
        return true;
    }

    bool FBox3DPhysicsScene::TryGetBodyState(ECS::FEntity Entity, FPhysicsBodyState& Out) const
    {
        return ReadBodyState(ResolveBody(FindEntityBody(Entity)), Out);
    }

    bool FBox3DPhysicsScene::TryGetTargetState(const FPhysicsBodyTarget& Target, FPhysicsBodyState& Out) const
    {
        return ReadBodyState(ResolveTarget(Target), Out);
    }

    void FBox3DPhysicsScene::QueueBodyCommand(const FBodyCommand& InCommand)
    {
        const bool bFinite = Box3DUtils::IsFiniteVector(InCommand.Value) && Box3DUtils::IsFiniteVector(InCommand.Point)
                          && Box3DUtils::IsFiniteVector(InCommand.Secondary) && Box3DUtils::IsFiniteVector(FVector3(InCommand.Parameters))
                          && Math::Abs(InCommand.Parameters.w) <= FLT_MAX;
        if (!bFinite)
        {
            static TAtomic<bool> bWarned{ false };
            if (!bWarned.exchange(true, std::memory_order_relaxed))
            {
                LOG_WARN("Dropped a physics body command with a NaN or infinite value for entity {}", InCommand.Entity.Value);
            }
            return;
        }

        FBodyCommand Command = InCommand;
        if (Command.Type != EBodyCommand::TargetForce)
        {
            const FBodyRecord* Record = FindBodyRecord(Command.Entity);
            Command.Revision = Record != nullptr ? Record->Revision : 0;
        }

        const uint32 Slot = Jobs::GetWorkerIndex();
        if (Slot < (uint32)ThreadBodyCommands.size())
        {
            ThreadBodyCommands[Slot].push_back(Command);
            return;
        }
        FScopeLock Lock(BodyCommandMutex);
        OverflowBodyCommands.push_back(Command);
    }

    void FBox3DPhysicsScene::ApplyBodyCommands()
    {
        BodyCommandScratch.clear();
        // Swapped out first, since a command such as buoyancy can queue more while it applies.
        const auto Drain = [this](TVector<FBodyCommand>& Commands)
        {
            DrainBodyCommands.clear();
            DrainBodyCommands.swap(Commands);
            for (const FBodyCommand& Command : DrainBodyCommands)
            {
                if (ApplyBodyCommand(Command) == EBodyCommandResult::Waiting)
                {
                    BodyCommandScratch.push_back(Command);
                }
            }
            if (Commands.empty())
            {
                DrainBodyCommands.clear();
                Commands.swap(DrainBodyCommands);
            }
        };

        // Waiting commands were queued first, so they run first to keep each body's order.
        Drain(WaitingBodyCommands);
        for (TVector<FBodyCommand>& Commands : ThreadBodyCommands)
        {
            Drain(Commands);
        }
        {
            FScopeLock Lock(BodyCommandMutex);
            Drain(OverflowBodyCommands);
        }
        WaitingBodyCommands.swap(BodyCommandScratch);
    }

    FBox3DPhysicsScene::EBodyCommandResult FBox3DPhysicsScene::ApplyBodyCommand(const FBodyCommand& Command)
    {
        b3BodyId Body = b3_nullBodyId;
        if (Command.Type == EBodyCommand::TargetForce)
        {
            Body = ResolveTarget(Command.Target);
        }
        else
        {
            const FBodyRecord* Record = FindBodyRecord(Command.Entity);
            if (Record == nullptr || Record->Status == EPhysicsBodyStatus::Failed
                || (Command.Revision != 0 && Command.Revision != Record->Revision))
            {
                return EBodyCommandResult::Dropped;
            }
            if (Record->Status == EPhysicsBodyStatus::Pending)
            {
                return EBodyCommandResult::Waiting;
            }
            Body = ResolveBody(Record->Handle);
        }
        if (!b3Body_IsValid(Body))
        {
            return EBodyCommandResult::Dropped;
        }
        const b3Vec3 Value = Box3DUtils::ToB3Vec3(Command.Value);
        const b3Vec3 Point = Box3DUtils::ToB3Vec3(Command.Point);
        switch (Command.Type)
        {
            case EBodyCommand::Impulse:
                b3Body_ApplyLinearImpulseToCenter(Body, Value, true);
                break;
            case EBodyCommand::Force:
                b3Body_ApplyForceToCenter(Body, Value, true);
                break;
            case EBodyCommand::Torque:
                b3Body_ApplyTorque(Body, Value, true);
                break;
            case EBodyCommand::AngularImpulse:
                b3Body_ApplyAngularImpulse(Body, Value, true);
                break;
            case EBodyCommand::LinearVelocity:
                b3Body_SetLinearVelocity(Body, Value);
                b3Body_SetAwake(Body, true);
                break;
            case EBodyCommand::AngularVelocity:
                b3Body_SetAngularVelocity(Body, Value);
                b3Body_SetAwake(Body, true);
                break;
            case EBodyCommand::ImpulseAtPosition:
                b3Body_ApplyLinearImpulse(Body, Value, Point, true);
                break;
            case EBodyCommand::ForceAtPosition:
            case EBodyCommand::TargetForce:
                b3Body_ApplyForce(Body, Value, Point, true);
                break;
            case EBodyCommand::Gravity:
                b3Body_SetGravityScale(Body, Command.Parameters.x);
                break;
            case EBodyCommand::Activate:
                b3Body_SetAwake(Body, true);
                break;
            case EBodyCommand::Deactivate:
                b3Body_SetAwake(Body, false);
                break;
            case EBodyCommand::MotionType:
            {
                const EBodyType Type = static_cast<EBodyType>(static_cast<uint8>(Command.Parameters.x));
                b3Body_SetType(Body, Box3DUtils::ToBox3DBodyType(Type));
                if (auto* Component = ECS::GetWorldRegistry(*World).TryGet<SRigidBodyComponent>(Command.Entity))
                {
                    Component->BodyType = Type;
                }
                break;
            }
            case EBodyCommand::SurfaceVelocity:
                ApplySurfaceVelocity(Body, Command.Value, Command.Secondary);
                break;
            case EBodyCommand::Buoyancy:
                ApplyBuoyancy(Command.Entity, Command.Point, Command.Secondary,
                    Command.Parameters.x, Command.Parameters.y, Command.Parameters.z, Command.Value, Command.Parameters.w);
                break;
        }
        return EBodyCommandResult::Applied;
    }

    void FBox3DPhysicsScene::AddForceAtTarget(const FPhysicsBodyTarget& Target, const FVector3& Force, const FVector3& Point)
    {
        FBodyCommand Command{ EBodyCommand::TargetForce };
        Command.Target = Target;
        Command.Value = Force;
        Command.Point = Point;
        QueueBodyCommand(Command);
    }

    void FBox3DPhysicsScene::SetSurfaceVelocity(ECS::FEntity Entity, const FVector3& Linear, const FVector3& Angular)
    {
        FBodyCommand Command{ EBodyCommand::SurfaceVelocity, Entity };
        Command.Value = Linear;
        Command.Secondary = Angular;
        QueueBodyCommand(Command);
    }

    void FBox3DPhysicsScene::ApplyBuoyancyImpulse(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime)
    {
        FBodyCommand Command{ EBodyCommand::Buoyancy, Entity };
        Command.Value = FluidVelocity;
        Command.Point = SurfacePosition;
        Command.Secondary = SurfaceNormal;
        Command.Parameters = FVector4(Buoyancy, LinearDrag, AngularDrag, DeltaTime);
        QueueBodyCommand(Command);
    }
}
