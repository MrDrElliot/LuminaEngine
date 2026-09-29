#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    EPhysicsBodyStatus FBox3DPhysicsScene::GetBodyStatus(ECS::FEntity Entity) const
    {
        if (auto It = RigidBodies.find(Entity); It != RigidBodies.end())
        {
            return It->second.Status;
        }
        if (auto It = CharacterBodies.find(Entity); It != CharacterBodies.end())
        {
            return It->second.Status;
        }
        return EPhysicsBodyStatus::Missing;
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

    void FBox3DPhysicsScene::QueueBodyCommand(FBodyCommand Command)
    {
        if (Command.Type != EBodyCommand::TargetForce)
        {
            const FBodyRecord* Record = nullptr;
            if (auto It = RigidBodies.find(Command.Entity); It != RigidBodies.end())
            {
                Record = &It->second;
            }
            else if (auto Character = CharacterBodies.find(Command.Entity); Character != CharacterBodies.end())
            {
                Record = &Character->second;
                Command.bCharacter = true;
            }
            if (Record == nullptr || Record->Status == EPhysicsBodyStatus::Failed)
            {
                return;
            }
            Command.Revision = Record->Revision;
        }
        FScopeLock Lock(BodyCommandMutex);
        PendingBodyCommands.push_back(Command);
    }

    void FBox3DPhysicsScene::ApplyBodyCommands()
    {
        BodyCommandScratch.clear();
        {
            FScopeLock Lock(BodyCommandMutex);
            BodyCommandScratch.swap(PendingBodyCommands);
        }
        size_t Waiting = 0;
        for (const FBodyCommand& Command : BodyCommandScratch)
        {
            b3BodyId Body = b3_nullBodyId;
            if (Command.Type == EBodyCommand::TargetForce)
            {
                Body = ResolveTarget(Command.Target);
            }
            else
            {
                const auto& Records = Command.bCharacter ? CharacterBodies : RigidBodies;
                auto It = Records.find(Command.Entity);
                if (It == Records.end() || It->second.Revision != Command.Revision
                    || It->second.Status == EPhysicsBodyStatus::Failed)
                {
                    continue;
                }
                if (It->second.Status == EPhysicsBodyStatus::Pending)
                {
                    BodyCommandScratch[Waiting++] = Command;
                    continue;
                }
                Body = ResolveBody(It->second.Handle);
            }
            if (!b3Body_IsValid(Body))
            {
                continue;
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
        }
        if (Waiting > 0)
        {
            FScopeLock Lock(BodyCommandMutex);
            PendingBodyCommands.insert(PendingBodyCommands.begin(), BodyCommandScratch.begin(), BodyCommandScratch.begin() + Waiting);
        }
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
