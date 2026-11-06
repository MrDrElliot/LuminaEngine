#include "RuntimePCH.h"
#include "CameraRigSystem.h"
#include "World/ECS/Registry.h"
#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/SpringArmComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/EntityTags.h"

namespace Lumina
{
    void SCameraRigSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
    }

    namespace CameraRigSystemInternal
    {
        // Frame-rate independent exponential smoothing weight. LagSpeed <= 0 snaps.
        static float SmoothAlpha(float LagSpeed, float Dt)
        {
            if (LagSpeed <= 0.0f)
            {
                return 1.0f;
            }
            return 1.0f - Math::Exp(-LagSpeed * Dt);
        }

        // Matches the camera convention where forward is rotation * (0,0,1).
        static FQuat MakeLookRotation(const FVector3& Forward, const FVector3& WorldUp)
        {
            const FVector3 F = Math::Normalize(Forward);
            FVector3 Up = WorldUp;
            if (Math::Abs(Math::Dot(F, Up)) > 0.999f)
            {
                Up = FVector3(0.0f, 0.0f, 1.0f);
                if (Math::Abs(Math::Dot(F, Up)) > 0.999f)
                {
                    Up = FVector3(1.0f, 0.0f, 0.0f);
                }
            }
            const FVector3 R = Math::Normalize(Math::Cross(Up, F));
            const FVector3 U = Math::Cross(F, R);
            return Math::ToQuat(FMatrix3(R, U, F));
        }

        static void ApplyWorldPose(STransformComponent& Xform, const FVector3& Location, const FQuat& Rotation)
        {
            FTransform World;
            World.SetLocation(Location);
            World.SetRotation(Rotation);
            World.SetScale(Xform.GetWorldScale());
            Xform.SetWorldTransform(World);
        }
    }

    bool SCameraRigSystem::HasWork(const SSystemContext&)
    {
        return AnyQueryHasRows(Followers, SpringArms);
    }

    void SCameraRigSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace CameraRigSystemInternal;

        const float Dt = Context.GetDeltaTime();
        Physics::IPhysicsQueries* PhysicsQueries = Context.GetPhysics();

        //~ Follow eases toward Target + Offset, optionally facing the target.
        Followers.ForEach([&](ECS::TMut<SCameraFollowComponent> FollowMut, ECS::TMut<STransformComponent> XformMut)
        {
            const STransformComponent* TargetXform = Targets.TryGet<STransformComponent>(FollowMut.Get().Target);
            if (TargetXform == nullptr)
            {
                return;
            }

            STransformComponent& Xform = XformMut.GetMut();
            SCameraFollowComponent& Follow = FollowMut.GetMut();
            const FVector3 TargetPos = TargetXform->GetWorldLocation();
            const FQuat TargetRot = TargetXform->GetWorldRotation();

            const FVector3 DesiredPos = Follow.bWorldSpaceOffset
                ? TargetPos + Follow.Offset
                : TargetPos + TargetRot * Follow.Offset;

            FQuat DesiredRot = Xform.GetWorldRotation();
            if (Follow.bLookAtTarget)
            {
                const FVector3 LookPoint = TargetPos + Follow.LookAtOffset;
                const FVector3 Dir = LookPoint - DesiredPos;
                if (Math::Dot(Dir, Dir) > 1e-6f)
                {
                    DesiredRot = MakeLookRotation(Dir, FVector3(0.0f, 1.0f, 0.0f));
                }
            }

            if (!Follow.bInitialized)
            {
                Follow.CurrentPosition = DesiredPos;
                Follow.CurrentRotation = DesiredRot;
                Follow.bInitialized = true;
            }
            else
            {
                Follow.CurrentPosition = Math::Mix(Follow.CurrentPosition, DesiredPos, SmoothAlpha(Follow.PositionLagSpeed, Dt));
                if (Follow.bLookAtTarget)
                {
                    FQuat To = DesiredRot;
                    if (Math::Dot(Follow.CurrentRotation, To) < 0.0f)
                    {
                        To = -To;
                    }
                    Follow.CurrentRotation = Math::Slerp(Follow.CurrentRotation, To, SmoothAlpha(Follow.RotationLagSpeed, Dt));
                }
                else
                {
                    Follow.CurrentRotation = DesiredRot;
                }
            }

            ApplyWorldPose(Xform, Follow.CurrentPosition, Follow.CurrentRotation);
        });

        //~ Spring arm places the camera a collision-shortened distance behind the pivot.
        SpringArms.ForEach([&](ECS::FEntity Entity, ECS::TMut<SSpringArmComponent> ArmMut, ECS::TMut<STransformComponent> XformMut)
        {
            STransformComponent& Xform = XformMut.GetMut();
            SSpringArmComponent& Arm = ArmMut.GetMut();
            const ECS::FEntity Target = Arm.Target;
            const STransformComponent* TargetXform = Targets.TryGet<STransformComponent>(Target);
            const bool bHasTarget = TargetXform != nullptr;

            FVector3 PivotBase;
            FQuat ControlRot;
            if (bHasTarget)
            {
                PivotBase  = TargetXform->GetWorldLocation();
                ControlRot = Arm.bUseControlRotation ? Xform.GetWorldRotation() : TargetXform->GetWorldRotation();
            }
            else
            {
                PivotBase  = Xform.GetWorldLocation();
                ControlRot = Xform.GetWorldRotation();
            }

            const FVector3 Pivot = PivotBase + Arm.TargetOffset;

            if (!Arm.bInitialized)
            {
                Arm.CurrentPivot     = Pivot;
                Arm.CurrentArmLength = Arm.TargetArmLength;
                Arm.bInitialized = true;
            }
            else
            {
                Arm.CurrentPivot = Math::Mix(Arm.CurrentPivot, Pivot, SmoothAlpha(Arm.PositionLagSpeed, Dt));
            }

            const FVector3 Back        = ControlRot * FVector3(0.0f, 0.0f, -1.0f);
            const FVector3 SocketWorld = ControlRot * Arm.SocketOffset;
            const FVector3 DesiredEnd  = Arm.CurrentPivot + Back * Arm.TargetArmLength + SocketWorld;

            float DesiredLength = Arm.TargetArmLength;
            if (PhysicsQueries != nullptr && Arm.bDoCollisionTest && Arm.TargetArmLength > 0.0f)
            {
                SSphereCastSettings Settings;
                Settings.Start  = Arm.CurrentPivot + SocketWorld;
                Settings.End    = DesiredEnd;
                Settings.Radius = Arm.ProbeSize;
                Settings.IgnoreEntities.Add(Entity);
                if (bHasTarget)
                {
                    Settings.IgnoreEntities.Add(Target);
                }

                const TOptional<SRayResult> Hit = PhysicsQueries->CastSphereClosest(Settings);
                const float Nearest = Hit.IsSet() ? Math::Min(1.0f, Hit->Fraction) : 1.0f;
                DesiredLength = Math::Max(0.0f, Arm.TargetArmLength * Nearest);
            }

            // Snap inward to avoid clipping; ease back out when the obstruction clears.
            if (DesiredLength < Arm.CurrentArmLength)
            {
                Arm.CurrentArmLength = DesiredLength;
            }
            else
            {
                Arm.CurrentArmLength = Math::Mix(Arm.CurrentArmLength, DesiredLength, SmoothAlpha(Arm.LengthLagSpeed, Dt));
            }

            const FVector3 FinalPos = Arm.CurrentPivot + Back * Arm.CurrentArmLength + SocketWorld;
            ApplyWorldPose(Xform, FinalPos, ControlRot);
        });
    }
}
