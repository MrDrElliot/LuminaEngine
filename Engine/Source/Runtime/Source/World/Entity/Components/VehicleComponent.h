#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "VehicleComponent.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EVehicleSteering : uint8
    {
        // Steered wheels turn, like a car.
        Wheels,
        // The two sides drive at different speeds, like a tank or a skid steer loader.
        Skid,
    };

    // One raycast wheel. The mount point is where the suspension is fully compressed, and the ray reaches down from it.
    REFLECT()
    struct RUNTIME_API FVehicleWheel
    {
        GENERATED_BODY()

        // Mount point in the body's local space, at the top of the suspension travel.
        PROPERTY(Editable, Category = "Wheel", Units = "m")
        FVector3 Position = FVector3(0.0f);

        PROPERTY(Editable, Category = "Wheel", ClampMin = 0.01f, Units = "m")
        float Radius = 0.4f;

        // How far the wheel drops below its mount point when the suspension is fully extended.
        PROPERTY(Editable, Category = "Wheel", ClampMin = 0.01f, Units = "m")
        float SuspensionTravel = 0.35f;

        PROPERTY(Editable, Category = "Wheel")
        bool bSteer = false;

        PROPERTY(Editable, Category = "Wheel")
        bool bDrive = true;

        // Locked by the handbrake, which also cuts its sideways grip so the vehicle can slide.
        PROPERTY(Editable, Category = "Wheel")
        bool bHandbrake = false;

        // Optional child entity posed each frame with the wheel's suspension, steering and spin.
        PROPERTY(Editable, Entity, Category = "Wheel")
        uint32 Visual = 0xFFFFFFFF;

        PROPERTY(ReadOnly, Category = "Wheel|State")
        bool bGrounded = false;

        // 0 with the suspension fully extended, 1 fully compressed.
        PROPERTY(ReadOnly, Category = "Wheel|State")
        float Compression = 0.0f;

        PROPERTY(ReadOnly, Category = "Wheel|State")
        FVector3 ContactPoint = FVector3(0.0f);

        PROPERTY(ReadOnly, Category = "Wheel|State")
        FVector3 ContactNormal = FVector3(0.0f, 1.0f, 0.0f);

        PROPERTY(ReadOnly, Entity, Category = "Wheel|State")
        uint32 GroundEntity = 0xFFFFFFFF;

        // Speed the tire slides at once its grip is exceeded, for skid marks and tire squeal.
        PROPERTY(ReadOnly, Category = "Wheel|State", Units = "m/s")
        float SlipSpeed = 0.0f;

        PROPERTY(ReadOnly, Category = "Wheel|State", Units = "deg")
        float SteerAngle = 0.0f;

        PROPERTY(ReadOnly, Category = "Wheel|State", Units = "deg")
        float SpinAngle = 0.0f;

        float PreviousCompressionLength = 0.0f;
        float SpinVelocity = 0.0f;
    };

    // Drives the dynamic rigid body on the same entity through raycast wheels, stepped with the physics.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SVehicleComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Vehicle")
        TVector<FVehicleWheel> Wheels;

        PROPERTY(Editable, Category = "Vehicle")
        EVehicleSteering Steering = EVehicleSteering::Wheels;

        // Natural frequency of the suspension, so the same tuning suits a light car and a heavy truck.
        PROPERTY(Editable, Category = "Vehicle|Suspension", ClampMin = 0.1f, Units = "Hz")
        float SuspensionFrequency = 1.6f;

        // 1 settles without bouncing, lower values let the body rock after a bump.
        PROPERTY(Editable, Category = "Vehicle|Suspension", ClampMin = 0.0f)
        float SuspensionDamping = 0.5f;

        PROPERTY(Editable, Category = "Vehicle|Engine", ClampMin = 0.0f, Units = "m/s2")
        float Acceleration = 9.0f;

        PROPERTY(Editable, Category = "Vehicle|Engine", ClampMin = 0.0f, Units = "m/s")
        float MaxSpeed = 30.0f;

        PROPERTY(Editable, Category = "Vehicle|Engine", ClampMin = 0.0f, Units = "m/s")
        float MaxReverseSpeed = 8.0f;

        PROPERTY(Editable, Category = "Vehicle|Engine", ClampMin = 0.0f, Units = "m/s2")
        float BrakeDeceleration = 16.0f;

        // Deceleration while coasting with no throttle.
        PROPERTY(Editable, Category = "Vehicle|Engine", ClampMin = 0.0f, Units = "m/s2")
        float RollingResistance = 1.0f;

        // Negative throttle brakes while moving forward and reverses once stopped, rather than needing a separate brake input.
        PROPERTY(Editable, Category = "Vehicle|Engine")
        bool bThrottleBrakes = true;

        // Friction coefficient between tire and ground, which caps how hard a wheel can push or corner.
        PROPERTY(Editable, Category = "Vehicle|Tires", ClampMin = 0.0f)
        float TireGrip = 1.2f;

        // Sideways grip kept on handbrake wheels while the handbrake is held.
        PROPERTY(Editable, Category = "Vehicle|Tires", ClampMin = 0.0f, ClampMax = 1.0f)
        float HandbrakeGrip = 0.35f;

        // Where sideways tire force acts, 0 at the center of mass height, which resists rolling over, and 1 at the contact point.
        PROPERTY(Editable, Category = "Vehicle|Tires", ClampMin = 0.0f, ClampMax = 1.0f)
        float RollInfluence = 0.25f;

        // Extra down force per squared unit of speed, which keeps a fast vehicle planted.
        PROPERTY(Editable, Category = "Vehicle|Tires", ClampMin = 0.0f, Units = "N")
        float Downforce = 0.0f;

        PROPERTY(Editable, Category = "Vehicle|Steering", ClampMin = 0.0f, ClampMax = 80.0f, Units = "deg")
        float MaxSteerAngle = 32.0f;

        PROPERTY(Editable, Category = "Vehicle|Steering", ClampMin = 0.0f, Units = "deg/s")
        float SteerSpeed = 160.0f;

        // Share of MaxSteerAngle left at MaxSpeed, so steering calms down as the vehicle speeds up.
        PROPERTY(Editable, Category = "Vehicle|Steering", ClampMin = 0.0f, ClampMax = 1.0f)
        float HighSpeedSteer = 0.4f;

        // Yaw rate a skid steer vehicle turns at with full steering input.
        PROPERTY(Editable, Category = "Vehicle|Steering", ClampMin = 0.0f, Units = "deg/s")
        float SkidTurnRate = 70.0f;

        // Forward drive, -1 to 1.
        PROPERTY(Editable, Category = "Vehicle|Input", ClampMin = -1.0f, ClampMax = 1.0f)
        float Throttle = 0.0f;

        // Positive steers right, -1 to 1.
        PROPERTY(Editable, Category = "Vehicle|Input", ClampMin = -1.0f, ClampMax = 1.0f)
        float Steer = 0.0f;

        PROPERTY(Editable, Category = "Vehicle|Input", ClampMin = 0.0f, ClampMax = 1.0f)
        float Brake = 0.0f;

        PROPERTY(Editable, Category = "Vehicle|Input")
        bool bHandbrake = false;

        // Signed speed along the vehicle's forward axis.
        PROPERTY(ReadOnly, Category = "Vehicle|State", Units = "m/s")
        float ForwardSpeed = 0.0f;

        PROPERTY(ReadOnly, Category = "Vehicle|State")
        int32 GroundedWheels = 0;

        FUNCTION()
        bool IsGrounded() const { return GroundedWheels > 0; }

        FUNCTION()
        void SetInput(float InThrottle, float InSteer, float InBrake = 0.0f, bool bInHandbrake = false)
        {
            Throttle   = Math::Clamp(InThrottle, -1.0f, 1.0f);
            Steer      = Math::Clamp(InSteer, -1.0f, 1.0f);
            Brake      = Math::Clamp(InBrake, 0.0f, 1.0f);
            bHandbrake = bInHandbrake;
        }

        FUNCTION()
        void AddWheel(FVector3 Position, float Radius, float SuspensionTravel, bool bSteer, bool bDrive, bool bInHandbrake = false,
                      uint32 Visual = 0xFFFFFFFF)
        {
            FVehicleWheel& Wheel = Wheels.emplace_back();
            Wheel.Position         = Position;
            Wheel.Radius           = Radius;
            Wheel.SuspensionTravel = SuspensionTravel;
            Wheel.bSteer           = bSteer;
            Wheel.bDrive           = bDrive;
            Wheel.bHandbrake       = bInHandbrake;
            Wheel.Visual           = Visual;
        }
    };
}
