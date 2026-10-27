#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "RagdollLocomotionComponent.generated.h"

namespace Lumina
{
    // Keeps a powered ragdoll on its feet and walks it with physics, so pushes, slopes and blows play out for real.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SRagdollLocomotionComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Input", ToolTip = "World direction to walk, its length the throttle from zero to one.")
        FVector3 MoveInput = FVector3(0.0f);

        PROPERTY(Editable, Category = "Input", ToolTip = "World direction to face. Zero faces where it walks.")
        FVector3 FacingDirection = FVector3(0.0f);

        PROPERTY(Editable, Category = "Locomotion", ClampMin = 0.0f, ClampMax = 10.0f, Units = "m/s")
        float MaxSpeed = 1.6f;

        PROPERTY(Editable, Category = "Locomotion", ClampMin = 0.0f, ClampMax = 40.0f, Units = "m/s2", ToolTip = "Most the legs can speed up or slow the body each second.")
        float MaxAcceleration = 3.0f;

        PROPERTY(Editable, Category = "Locomotion|Gait", ToolTip = "Steps the legs to wherever keeps the body over its feet and swings the arms. Off leaves the limbs to the animation.")
        bool bProceduralGait = true;

        PROPERTY(Editable, Category = "Locomotion|Gait", ClampMin = 0.1f, ClampMax = 1.5f, ToolTip = "Thigh swing, in radians, at which the arms reach their full swing.")
        float HipSwing = 0.45f;

        PROPERTY(Editable, Category = "Locomotion|Gait", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Knee bend at the top of a step, in radians.")
        float KneeLift = 0.6f;

        PROPERTY(Editable, Category = "Locomotion|Gait", ClampMin = 0.0f, ClampMax = 1.5f, ToolTip = "Arm swing against the legs, in radians.")
        float ArmSwing = 0.4f;

        PROPERTY(Editable, Category = "Locomotion|Gait", ToolTip = "Direction the character faces in its own skeleton space, used when its feet have no toe bones to tell.")
        FVector3 ForwardAxis = FVector3(0.0f, 0.0f, 1.0f);

        PROPERTY(Editable, Category = "Balance", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Strength of the reflexes that keep the body upright, at its height and turned to face.")
        float BalanceStrength = 1.0f;

        PROPERTY(Editable, Category = "Balance", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Share of the body's weight the balance carries, the rest pressing the feet into the ground for grip.")
        float SupportFraction = 0.9f;

        PROPERTY(Editable, Category = "Balance", ClampMin = 5.0f, ClampMax = 90.0f, Units = "deg", ToolTip = "Lean past which the body gives up and falls.")
        float FallAngle = 55.0f;

        PROPERTY(Editable, Category = "Balance", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Muscle strength left while fallen.")
        float FallenStrength = 0.15f;

        PROPERTY(Editable, Category = "Balance", ToolTip = "Gets back up on its own after a fall.")
        bool bAutoRecover = true;

        PROPERTY(Editable, Category = "Balance", ClampMin = 0.0f, ClampMax = 30.0f, Units = "s", EditCondition = "bAutoRecover", ToolTip = "Time spent down, once at rest, before getting up.")
        float RecoverDelay = 1.5f;

        PROPERTY(Editable, Category = "Balance", ClampMin = 0.1f, ClampMax = 10.0f, Units = "s", EditCondition = "bAutoRecover")
        float RecoverDuration = 1.5f;

        PROPERTY(ReadOnly, NoSerialize, Category = "State")
        bool bFallen = false;

        PROPERTY(ReadOnly, NoSerialize, Category = "State")
        bool bGrounded = false;

        PROPERTY(ReadOnly, NoSerialize, Category = "State", Units = "m/s")
        float Speed = 0.0f;

        PROPERTY(ReadOnly, NoSerialize, Category = "State", ToolTip = "Progress of getting back up, from zero to one.")
        float Recovery = 0.0f;
    };
}
