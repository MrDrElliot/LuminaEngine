#pragma once

#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Core/Math/Sine.h"
#include "InterpolatingMovementComponent.generated.h"

namespace Lumina
{
    
    REFLECT(Component, Category = "Movement")
    struct RUNTIME_API SInterpolatingMovementComponent
    {
        GENERATED_BODY()

        /** Local-space transform the entity interpolates from. */
        PROPERTY(Editable, Category = "Interp")
        FTransform Start;

        /** Local-space transform the entity interpolates toward. */
        PROPERTY(Editable, Category = "Interp")
        FTransform End;

        /** Normalized speed of interpolation (1.0 completes a full cycle per second). */
        PROPERTY(Editable, Category = "Interp")
        float Speed = 1.0f;

        FTransform OriginTransform;
        float Alpha = 0.0f;
        bool bForward = true;
    };

    REFLECT(Component, Category = "Movement")
    struct RUNTIME_API SSineWaveMovementComponent
    {
        GENERATED_BODY()
    
        /** World-space axis the entity oscillates along. */
        PROPERTY(Editable, Category = "Wave")
        FVector3 Axis = FVector3(0.0f, 1.0f, 0.0f);

        /** Peak displacement distance from the rest position (meters). */
        PROPERTY(Editable, Category = "Wave")
        float Amplitude = 1.0f;

        /** Oscillation cycles per second. */
        PROPERTY(Editable, Category = "Wave")
        float Frequency = 1.0f;

        /** Starting phase offset in radians. */
        PROPERTY(Editable, Category = "Wave")
        float PhaseOffset = 0.0f;

        /** When true, a second axis wave is layered on top of the primary. */
        PROPERTY(Editable, Category = "Multi-Axis")
        bool bEnableSecondaryAxis = false;

        /** World-space axis for the secondary wave. */
        PROPERTY(Editable, Category = "Multi-Axis")
        FVector3 SecondaryAxis = FVector3(1.0f, 0.0f, 0.0f);

        /** Peak displacement of the secondary wave (meters). */
        PROPERTY(Editable, Category = "Multi-Axis")
        float SecondaryAmplitude = 0.5f;

        /** Cycles per second for the secondary wave. */
        PROPERTY(Editable, Category = "Multi-Axis")
        float SecondaryFrequency = 1.5f;

        /** Phase offset for the secondary wave (radians). */
        PROPERTY(Editable, Category = "Multi-Axis")
        float SecondaryPhaseOffset = 0.0f;

        /** Waveform shape applied to the oscillation (Sine, Square, Sawtooth, etc.). */
        PROPERTY(Editable, Category = "Modifiers")
        ESineWaveType WaveType = ESineWaveType::Sine;

        /** When true, amplitude decays exponentially over time. */
        PROPERTY(Editable, Category = "Modifiers")
        bool bDampingEnabled = false;

        /** Exponential decay rate when damping is enabled (higher = faster decay). */
        PROPERTY(Editable, Category = "Modifiers")
        float DampingFactor = 0.1f;

        /** When true, amplitude is modulated by a secondary low-frequency oscillator. */
        PROPERTY(Editable, Category = "Modifiers")
        bool bAmplitudeModulation = false;

        /** Frequency of the amplitude modulation envelope (Hz). */
        PROPERTY(Editable, Category = "Modifiers")
        float AmplitudeModulationFrequency = 0.5f;

        /** When true, offsets are relative to the entity's initial transform rather than world origin. */
        PROPERTY(Editable, Category = "Behavior")
        bool bUseRelativeMovement = true;

        /** When true, oscillation begins immediately when the entity is spawned. */
        PROPERTY(Editable, Category = "Behavior")
        bool bAutoStart = true;

        /** Seconds to wait before the oscillation begins. */
        PROPERTY(Editable, Category = "Behavior")
        float StartDelay = 0.0f;
    
        FVector3 InitialPosition = FVector3(0.0f);
        float CurrentTime = 0.0f;
        float CurrentAmplitude = 0.0f;
        bool bInitialized = false;
        bool bIsActive = false;
    };
    
}
