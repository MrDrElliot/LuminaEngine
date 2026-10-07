#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include "Box3DInternal.h"
#include "Box3DPhysics.h"
#include "Box3DUtils.h"

#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VehicleComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Below this the vehicle counts as stopped, so negative throttle reverses and an idle vehicle holds on a slope.
        constexpr float StoppedSpeed = 0.5f;

        // Short of canceling all sideways speed in one step, which would ring against the next step's correction.
        constexpr float LateralResponse = 0.9f;

        constexpr int32 MaxWheels = 16;

        struct FWheelRay
        {
            b3BodyId            IgnoreBody{};
            FCollisionProfile   Profile{};
            bool                bPermissive = true;
            b3ShapeId           Shape = b3_nullShapeId;
            b3Vec3              Point{};
            b3Vec3              Normal{};
            float               Fraction = 1.0f;
            bool                bHit = false;
        };

        // Characters are skipped, so a wheel meets a soldier through the chassis collider rather than climbing onto the capsule.
        float WheelRayCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction, uint64_t, int, int, void* Context)
        {
            FWheelRay& Ray = *static_cast<FWheelRay*>(Context);

            if (B3_ID_EQUALS(b3Shape_GetBody(ShapeId), Ray.IgnoreBody)
                || Box3DUtils::IsCharacterProxyUserData(b3Shape_GetUserData(ShapeId))
                || !Box3DUtils::ShouldProfileCollideWithShape(Ray.Profile, ShapeId, Ray.bPermissive))
            {
                return -1.0f;
            }

            Ray.Shape    = ShapeId;
            Ray.Point    = Point;
            Ray.Normal   = Normal;
            Ray.Fraction = Fraction;
            Ray.bHit     = true;
            return Fraction;
        }

        FVector3 FromB3(const b3Vec3& V)
        {
            return Box3DUtils::FromB3Vec3(V);
        }

        b3Vec3 ToB3(const FVector3& V)
        {
            return Box3DUtils::ToB3Vec3(V);
        }

        // Opposes a speed without overshooting it, so a stopping force never pushes the vehicle back the other way.
        float OpposeSpeed(float Speed, float Force, float MassShare, float Dt)
        {
            const float Stop = MassShare * Math::Abs(Speed) / Dt;
            return -Math::Sign(Speed) * Math::Min(Force, Stop);
        }

        float SteerLimit(const SVehicleComponent& Vehicle, float Speed)
        {
            const float SpeedRatio = Vehicle.MaxSpeed > 0.0f ? Math::Clamp(Math::Abs(Speed) / Vehicle.MaxSpeed, 0.0f, 1.0f) : 0.0f;
            return Vehicle.MaxSteerAngle * Math::Lerp(1.0f, Vehicle.HighSpeedSteer, SpeedRatio);
        }
    }

    void FBox3DPhysicsScene::UpdateVehicles(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const bool bDebugDraw = FBox3DPhysicsContext::IsDebugDrawEnabled();
        const bool bPermissive = Box3DUtils::UsesPermissiveCollisionFilter();

        Registry.View<SVehicleComponent, SRigidBodyComponent>().ForEach([&](ECS::FEntity Entity, SVehicleComponent& Vehicle, const SRigidBodyComponent& RigidBody)
        {
            const b3BodyId Body = ResolveBody(FindEntityBody(Entity));
            if (!b3Body_IsValid(Body) || b3Body_GetType(Body) != b3_dynamicBody || Vehicle.Wheels.empty())
            {
                return;
            }

            const bool bHasInput = Math::Abs(Vehicle.Throttle) > 0.01f || Math::Abs(Vehicle.Steer) > 0.01f;
            if (!b3Body_IsAwake(Body))
            {
                if (!bHasInput)
                {
                    return;
                }
                b3Body_SetAwake(Body, true);
            }

            const float Mass = b3Body_GetMass(Body);
            if (Mass <= 0.0f)
            {
                return;
            }

            const FVector3 Up       = FromB3(b3Body_GetWorldVector(Body, b3Vec3{ 0.0f, 1.0f, 0.0f }));
            const FVector3 Forward  = FromB3(b3Body_GetWorldVector(Body, b3Vec3{ 0.0f, 0.0f, 1.0f }));
            const FVector3 Center   = FromB3(b3Body_GetWorldCenter(Body));
            const FVector3 Velocity = FromB3(b3Body_GetLinearVelocity(Body));
            const float Speed = Math::Dot(Velocity, Forward);
            Vehicle.ForwardSpeed = Speed;

            // Negative throttle brakes a vehicle still rolling forward, and positive throttle one rolling back.
            float Drive = Vehicle.Throttle;
            float BrakeInput = Vehicle.Brake;
            if (Vehicle.bThrottleBrakes && Math::Abs(Speed) > StoppedSpeed && Drive * Speed < 0.0f)
            {
                BrakeInput = Math::Max(BrakeInput, Math::Abs(Drive));
                Drive = 0.0f;
            }
            if (Math::Abs(Drive) < 0.01f && Math::Abs(Speed) < StoppedSpeed && Vehicle.Steering == EVehicleSteering::Wheels)
            {
                BrakeInput = 1.0f;
            }

            const float TopSpeed = Drive >= 0.0f ? Vehicle.MaxSpeed : Vehicle.MaxReverseSpeed;
            const float SpeedFalloff = TopSpeed > 0.0f ? Math::Clamp(1.0f - Math::Abs(Speed) / TopSpeed, 0.0f, 1.0f) : 0.0f;
            const float DriveAcceleration = Drive * Vehicle.Acceleration * SpeedFalloff;

            const float TargetSteer = Vehicle.Steer * SteerLimit(Vehicle, Speed);
            const float SteerStep = Vehicle.SteerSpeed * FixedDt;
            const float YawRate = Vehicle.Steering == EVehicleSteering::Skid ? Math::Radians(Vehicle.Steer * Vehicle.SkidTurnRate) : 0.0f;

            const int32 WheelCount = Math::Min((int32)Vehicle.Wheels.size(), MaxWheels);
            const float SuspensionMass = Mass / (float)WheelCount;
            const float Omega = Math::TwoPi<float>() * Vehicle.SuspensionFrequency;
            const float Stiffness = SuspensionMass * Omega * Omega;
            const float Damping = 2.0f * Vehicle.SuspensionDamping * Math::Sqrt(Stiffness * SuspensionMass);

            const b3QueryFilter Filter = Box3DUtils::MakeQueryFilter(RigidBody.CollisionProfile);

            // Every wheel reports ground first, since how the weight splits depends on how many are down.
            int32 Grounded = 0;
            int32 DrivenGrounded = 0;
            FWheelRay Rays[MaxWheels];
            for (int32 Index = 0; Index < WheelCount; ++Index)
            {
                FVehicleWheel& Wheel = Vehicle.Wheels[Index];
                const float Reach = Wheel.SuspensionTravel + Wheel.Radius;
                const b3Pos Mount = b3Body_GetWorldPoint(Body, ToB3(Wheel.Position));

                FWheelRay& Ray = Rays[Index];
                Ray = FWheelRay{};
                Ray.IgnoreBody  = Body;
                Ray.Profile     = RigidBody.CollisionProfile;
                Ray.bPermissive = bPermissive;
                b3World_CastRay(WorldId, Mount, ToB3(-Up * Reach), Filter, &WheelRayCallback, &Ray);

                Wheel.bGrounded = Ray.bHit;
                if (Ray.bHit)
                {
                    ++Grounded;
                    DrivenGrounded += Wheel.bDrive ? 1 : 0;
                }

                const float WheelSteer = Wheel.bSteer ? TargetSteer * (Wheel.Position.z < 0.0f ? -1.0f : 1.0f) : 0.0f;
                Wheel.SteerAngle += Math::Clamp(WheelSteer - Wheel.SteerAngle, -SteerStep, SteerStep);

                if (bDebugDraw)
                {
                    const FVector3 From = FromB3(Mount);
                    const FVector3 To = Ray.bHit ? FromB3(Ray.Point) : From - Up * Reach;
                    World->DrawLine(From, To, Ray.bHit ? FVector4(0.2f, 1.0f, 0.3f, 1.0f) : FVector4(1.0f, 0.3f, 0.2f, 1.0f), 1.0f, false, 0.0f);
                }
            }

            Vehicle.GroundedWheels = Grounded;
            const float MassShare = Grounded > 0 ? Mass / (float)Grounded : Mass;
            const float DownforceShare = Grounded > 0 ? Vehicle.Downforce * Speed * Speed / (float)Grounded : 0.0f;

            for (int32 Index = 0; Index < WheelCount; ++Index)
            {
                FVehicleWheel& Wheel = Vehicle.Wheels[Index];
                const FWheelRay& Ray = Rays[Index];
                const bool bLocked = Vehicle.bHandbrake && Wheel.bHandbrake;

                if (!Ray.bHit)
                {
                    Wheel.Compression = 0.0f;
                    Wheel.PreviousCompressionLength = 0.0f;
                    Wheel.GroundEntity = ECS::NullEntity;
                    Wheel.SlipSpeed = 0.0f;
                    const float FreeSpin = Wheel.bDrive && !bLocked ? Drive * Vehicle.MaxSpeed / Wheel.Radius : 0.0f;
                    Wheel.SpinVelocity = Math::Lerp(Wheel.SpinVelocity, FreeSpin, Math::Min(1.0f, 2.0f * FixedDt));
                    Wheel.SpinAngle = Math::Mod(Wheel.SpinAngle + Math::Degrees(Wheel.SpinVelocity) * FixedDt, 360.0f);
                    continue;
                }

                const float Reach = Wheel.SuspensionTravel + Wheel.Radius;
                const float Length = Math::Max(Ray.Fraction * Reach - Wheel.Radius, 0.0f);
                const float CompressionLength = Wheel.SuspensionTravel - Length;
                const float CompressionSpeed = (CompressionLength - Wheel.PreviousCompressionLength) / FixedDt;
                Wheel.PreviousCompressionLength = CompressionLength;
                Wheel.Compression = Math::Clamp(CompressionLength / Wheel.SuspensionTravel, 0.0f, 1.0f);

                const float Load = Math::Max(Stiffness * CompressionLength + Damping * CompressionSpeed, 0.0f) + DownforceShare;

                const FVector3 Contact = FromB3(Ray.Point);
                const FVector3 Normal = FromB3(Ray.Normal);
                Wheel.ContactPoint = Contact;
                Wheel.ContactNormal = Normal;

                const b3BodyId Ground = b3Shape_GetBody(Ray.Shape);
                Wheel.GroundEntity = EntityOfBody(Ground);

                // The tire frame lies in the contact plane, turned by the wheel's steering.
                const float SteerRadians = Math::Radians(Wheel.SteerAngle);
                const FVector3 LocalHeading(Math::Sin(SteerRadians), 0.0f, Math::Cos(SteerRadians));
                FVector3 Heading = FromB3(b3Body_GetWorldVector(Body, ToB3(LocalHeading)));
                Heading = Math::Normalize(Heading - Normal * Math::Dot(Heading, Normal));
                const FVector3 Side = Math::Cross(Normal, Heading);

                // Relative to the ground under the wheel, so a vehicle parked on a moving platform rides along.
                FVector3 PointVelocity = FromB3(b3Body_GetWorldPointVelocity(Body, Ray.Point));
                if (b3Body_GetType(Ground) != b3_staticBody)
                {
                    PointVelocity -= FromB3(b3Body_GetWorldPointVelocity(Ground, Ray.Point));
                }
                const FVector3 Arm = Contact - Center;
                if (YawRate != 0.0f)
                {
                    // Tracks carry the commanded turn, so friction steers the hull toward that yaw rate rather than resisting it.
                    PointVelocity -= Math::Cross(Up * YawRate, Arm);
                }

                const float LongSpeed = Math::Dot(PointVelocity, Heading);
                const float LatSpeed = Math::Dot(PointVelocity, Side);

                float LongForce = 0.0f;
                if (Wheel.bDrive && DrivenGrounded > 0 && !bLocked)
                {
                    LongForce += DriveAcceleration * Mass / (float)DrivenGrounded;
                }

                const float WheelBrake = bLocked ? 1.0f : BrakeInput;
                if (WheelBrake > 0.0f)
                {
                    LongForce += OpposeSpeed(LongSpeed, Vehicle.BrakeDeceleration * WheelBrake * MassShare, MassShare, FixedDt);
                }
                else if (Math::Abs(Drive) < 0.01f)
                {
                    LongForce += OpposeSpeed(LongSpeed, Vehicle.RollingResistance * MassShare, MassShare, FixedDt);
                }

                const float Grip = Vehicle.TireGrip * Load;
                const float LatLimit = bLocked ? Grip * Vehicle.HandbrakeGrip : Grip;
                float LatForce = Math::Clamp(-LatSpeed * MassShare * LateralResponse / FixedDt, -LatLimit, LatLimit);

                // Past the friction circle the tire slides, which is what makes a hard turn at speed drift.
                const float Total = Math::Sqrt(LongForce * LongForce + LatForce * LatForce);
                Wheel.SlipSpeed = 0.0f;
                if (Total > Grip && Total > 0.0f)
                {
                    const float Scale = Grip / Total;
                    LongForce *= Scale;
                    LatForce *= Scale;
                    Wheel.SlipSpeed = Math::Abs(LatSpeed);
                }
                else if (Math::Abs(LatForce) >= LatLimit)
                {
                    Wheel.SlipSpeed = Math::Abs(LatSpeed);
                }

                // Raised toward the center of mass, since tire force at ground level would tip the vehicle over in every turn.
                const FVector3 TireOrigin = Center + Arm - Up * (Math::Dot(Arm, Up) * (1.0f - Vehicle.RollInfluence));
                const FVector3 TireForce = Heading * LongForce + Side * LatForce;
                const FVector3 SuspensionForce = Up * Load;

                b3Body_ApplyForce(Body, ToB3(SuspensionForce), Ray.Point, false);
                b3Body_ApplyForce(Body, ToB3(TireForce), ToB3(TireOrigin), false);
                if (b3Body_GetType(Ground) == b3_dynamicBody)
                {
                    b3Body_ApplyForce(Ground, ToB3(-(SuspensionForce + TireForce)), Ray.Point, true);
                }

                Wheel.SpinVelocity = bLocked ? 0.0f : LongSpeed / Wheel.Radius;
                Wheel.SpinAngle = Math::Mod(Wheel.SpinAngle + Math::Degrees(Wheel.SpinVelocity) * FixedDt, 360.0f);
            }
        });
    }

    void FBox3DPhysicsScene::PoseVehicleWheels()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.View<SVehicleComponent>().ForEach([&](ECS::FEntity, const SVehicleComponent& Vehicle)
        {
            for (const FVehicleWheel& Wheel : Vehicle.Wheels)
            {
                const ECS::FEntity Visual = Wheel.Visual;
                STransformComponent* Transform = Visual != ECS::NullEntity ? Registry.TryGet<STransformComponent>(Visual) : nullptr;
                if (Transform == nullptr)
                {
                    continue;
                }

                const float Length = Wheel.SuspensionTravel * (1.0f - Wheel.Compression);
                const FQuat Steer = Math::AngleAxis(Math::Radians(Wheel.SteerAngle), FVector3(0.0f, 1.0f, 0.0f));
                const FQuat Spin = Math::AngleAxis(Math::Radians(Wheel.SpinAngle), FVector3(1.0f, 0.0f, 0.0f));

                FTransform Local = Transform->LocalTransform;
                Local.SetLocation(Wheel.Position - FVector3(0.0f, Length, 0.0f));
                Local.SetRotation(Steer * Spin);
                Transform->SetLocalTransform(Local);
            }
        });
    }
}
