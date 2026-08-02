#pragma once

#include "Core/Object/ObjectMacros.h"
#include "CharacterControllerComponent.generated.h"


namespace Lumina
{
    REFLECT(Component, Category = "Character")
    struct RUNTIME_API SCharacterControllerComponent
    {
        GENERATED_BODY()
        
        FUNCTION(Script)
        void AddMovementInput(const FVector3& Move)
        {
            MoveInput += Move;
        }
        
        FUNCTION(Script)
        void AddLookInput(const FVector2& Look)
        {
            LookInput.x += Look.x;
            LookInput.y = Math::Clamp(LookInput.y + Look.y, PitchClamp.x, PitchClamp.y);
        }
        
        FUNCTION(Script)
        void AddPitch(float Degrees)
        {
            LookInput.y = Math::Clamp(LookInput.y + Degrees, PitchClamp.x, PitchClamp.y);
        }
        
        FUNCTION(Script)
        void AddYaw(float Degrees)
        {
            LookInput.x += Degrees;
        }
        
        FUNCTION(Script)
        FVector3 GetLookForward() const
        {
            float Pitch = Math::Radians(-LookInput.y);
            float Yaw   = Math::Radians(LookInput.x);

            return Math::Normalize(FVector3(
                Math::Cos(Pitch) * Math::Sin(Yaw),
                Math::Sin(Pitch),
                Math::Cos(Pitch) * Math::Cos(Yaw)
            ));
        }
        
        FUNCTION(Script)
        FVector3 GetLookRight() const
        {
            float Yaw = Math::Radians(LookInput.x);
        
            return Math::Normalize(FVector3(
                Math::Cos(Yaw),
                0.0f,
                -Math::Sin(Yaw)
            ));
        }
        
        FUNCTION(Script)
        void Jump() { bJumpPressed = true; }

        // Add a velocity impulse (jump pad, knockback, dash). Override flags replace velocity on that axis
        // instead of adding: horizontal = X/Z plane, vertical = Y (up).
        FUNCTION(Script)
        void Launch(const FVector3& Velocity, bool bOverrideHorizontal, bool bOverrideVertical)
        {
            PendingLaunchVelocity     = Velocity;
            bLaunchOverrideHorizontal = bOverrideHorizontal;
            bLaunchOverrideVertical   = bOverrideVertical;
            bLaunchRequested          = true;
        }

        // Move the character to a world location. Use this, not a direct transform write -- the physics
        // capsule owns position and a plain write is overwritten next physics step.
        FUNCTION(Script)
        void TeleportTo(const FVector3& Location)
        {
            PendingTeleportLocation = Location;
            bTeleportRequested      = true;
        }

        /** Accumulated movement input vector, consumed each physics frame. */
        PROPERTY(Script, ReadOnly)
        FVector3 MoveInput;

        /** Accumulated look input (X = yaw degrees, Y = pitch degrees). */
        PROPERTY(Script, ReadOnly)
        FVector2 LookInput;

        /** Minimum and maximum pitch angles (degrees) the look input is clamped to. */
        PROPERTY(Script, Editable)
        FVector2 PitchClamp = FVector2(-89.9, 89.9);

        /** True for one frame after Jump() is called; consumed by the movement system. */
        PROPERTY(Script, ReadOnly)
        bool bJumpPressed = false;

        // Transient Launch/Teleport requests, latched into the movement
        // component before the physics step (same path as bJumpPressed).
        FVector3 PendingLaunchVelocity     = FVector3(0.0f);
        bool      bLaunchRequested          = false;
        bool      bLaunchOverrideHorizontal = false;
        bool      bLaunchOverrideVertical   = false;
        FVector3 PendingTeleportLocation   = FVector3(0.0f);
        bool      bTeleportRequested        = false;
    };
}
