#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/Key.h"
#include "InputAction.generated.h"

namespace Lumina
{
    // Every type reads the same bindings, so a digital action still reports 1 or 0 as a value.
    REFLECT()
    enum class EInputActionType : uint8
    {
        Digital,
        Axis1D,
        Axis2D,
    };

    // Where a binding's value comes from. Key contributes Scale while its SKey is held, and the rest ignore the SKey.
    REFLECT()
    enum class EInputAxisSource : uint8
    {
        Key,

        // This frame's motion in pixels, so a look action reads them without scaling by frame time.
        MouseX,
        MouseY,
        MouseWheel,

        // A position from -1 to 1 with up positive, so a look action scales them by frame time to get a turn rate.
        GamepadLeftX,
        GamepadLeftY,
        GamepadRightX,
        GamepadRightY,

        // From 0 at rest to 1 fully pulled.
        GamepadLeftTrigger,
        GamepadRightTrigger,
    };

    inline bool IsGamepadSource(EInputAxisSource Source)
    {
        return Source >= EInputAxisSource::GamepadLeftX && Source <= EInputAxisSource::GamepadRightTrigger;
    }

    // The channel of an Axis2D action a binding feeds. The other types only use X.
    REFLECT()
    enum class EInputAxisChannel : uint8
    {
        X,
        Y,
    };

    // One physical binding for an action, a button on any device or a continuous source, plus the scale it contributes.
    REFLECT()
    struct RUNTIME_API SInputActionBinding
    {
        GENERATED_BODY()

        SInputActionBinding() = default;
        explicit SInputActionBinding(const SKey& InKey, float InScale = 1.0f)
            : Key(InKey), Scale(InScale) {}

        PROPERTY(Editable)
        SKey Key;

        // The value while Key is held, such as 1 or -1, or the multiplier on a continuous source.
        PROPERTY(Editable)
        float Scale = 1.0f;

        PROPERTY(Editable)
        EInputAxisSource Source = EInputAxisSource::Key;

        // Axis2D only. W and S on Y with A and D on X make a movement stick.
        PROPERTY(Editable)
        EInputAxisChannel Channel = EInputAxisChannel::X;

        // Gamepad axis sources only, applied on top of the project-wide stick dead zone.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 0.99f)
        float DeadZone = 0.0f;

        // Gamepad axis sources only. Above one gives finer control near center, such as 2 for a squared look curve.
        PROPERTY(Editable, ClampMin = 0.1f, ClampMax = 5.0f)
        float ResponseExponent = 1.0f;

        // Such as "Ctrl+S", "Mouse X" or "Gamepad Left Stick Y", and empty for a Key source with no key bound.
        FString GetDisplayName(EGamepadFamily Family = EGamepadFamily::Xbox) const;
    };

    // NoCSharp because this is the authored definition, and LuminaSharp.SInputAction already names the script-side binding.
    REFLECT(NoCSharp)
    struct RUNTIME_API SInputAction
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        PROPERTY(Editable)
        EInputActionType Type = EInputActionType::Digital;

        // Keeps firing while the context is in UI mode, for pause and save hotkeys.
        PROPERTY(Editable)
        bool bRunsInUI = false;

        // Input below this reads as zero and the rest is rescaled to full range, radially for Axis2D.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 0.99f)
        float DeadZone = 0.0f;

        // Multiplies the shaped value, the usual home for look sensitivity.
        PROPERTY(Editable, ClampMin = 0.0f)
        float Sensitivity = 1.0f;

        PROPERTY(Editable)
        bool bInvert = false;

        // Seconds down before it counts as held, where zero means held from the first frame.
        PROPERTY(Editable, ClampMin = 0.0f)
        float HoldTime = 0.0f;

        // A press released within this many seconds also reports a tap on the release frame.
        PROPERTY(Editable, ClampMin = 0.0f)
        float TapTime = 0.2f;

        // A second press within this many seconds of the first reports a double tap, and zero turns it off.
        PROPERTY(Editable, Category = "Triggers", ClampMin = 0.0f)
        float DoubleTapTime = 0.0f;

        // Seconds held before the first repeat, as a menu starts scrolling. Zero repeats at RepeatInterval from the press.
        PROPERTY(Editable, Category = "Triggers", ClampMin = 0.0f)
        float RepeatDelay = 0.0f;

        // Seconds between repeats while held, and zero turns repeating off.
        PROPERTY(Editable, Category = "Triggers", ClampMin = 0.0f)
        float RepeatInterval = 0.0f;

        // Fires only while this other action is also down, such as a shoulder button that changes what the face buttons do.
        PROPERTY(Editable, Category = "Triggers")
        FName ChordAction;

        // Keeps a press available for this many seconds until gameplay consumes it, so a jump just before landing still lands.
        PROPERTY(Editable, Category = "Triggers", ClampMin = 0.0f)
        float BufferTime = 0.0f;

        PROPERTY(Editable)
        TVector<SInputActionBinding> Bindings;

        // Actions saved before EInputActionType existed carry this instead, until FoldLegacyAxisFlag turns it into Type.
        PROPERTY(Hidden)
        bool bAxis = false;

        void FoldLegacyAxisFlag()
        {
            if (bAxis && Type == EInputActionType::Digital)
            {
                Type = EInputActionType::Axis1D;
            }
            bAxis = false;
        }
    };

    // A layer a world pushes to change what input means, such as a pause menu that only lets its own actions through.
    REFLECT()
    struct RUNTIME_API SInputMappingContext
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        /** The actions this layer allows. An action listed here fires while the layer is on the stack. */
        PROPERTY(Editable)
        TVector<FName> Actions;

        // Stops every action this layer does not list from reaching layers and gameplay below it.
        PROPERTY(Editable)
        bool bBlockLower = true;
    };

    // A reference to an authored action, resolved once per settings change instead of hashed per query.
    REFLECT()
    struct RUNTIME_API SInputActionHandle
    {
        GENERATED_BODY()

        SInputActionHandle() = default;
        explicit SInputActionHandle(FName InName) : Name(InName) {}

        // Public so the property system can write it; the resolve re-checks it against CachedName.
        PROPERTY(Editable)
        FName Name;

        FName GetName() const { return Name; }
        void  SetName(FName InName) { Name = InName; }

        bool IsSet() const { return !Name.IsNone(); }

    private:

        friend class FInputActionMap;

        mutable int32  CachedIndex = Constants::kIndexNone;
        mutable uint32 CachedSerial = 0;
        mutable FName  CachedName;
    };

    // One action's state this frame, mirrored byte for byte by LuminaSharp so scripts read the whole array through a pointer.
    REFLECT()
    struct SInputActionState
    {
        GENERATED_BODY()

        enum EFlags : uint32
        {
            Flag_Down     = 1u << 0,  ///< Down this frame.
            Flag_Pressed  = 1u << 1,  ///< Went down this frame.
            Flag_Released = 1u << 2,  ///< Came up this frame.
            Flag_Held     = 1u << 3,  ///< Down and past the action's HoldTime.
            Flag_Tapped   = 1u << 4,  ///< Released this frame after less than TapTime down.
            Flag_DoubleTapped = 1u << 5,  ///< Pressed this frame within DoubleTapTime of the previous press.
            Flag_Repeated     = 1u << 6,  ///< Pressed this frame, or a repeat came due while held.
            Flag_Buffered     = 1u << 7,  ///< Pressed within BufferTime and not yet consumed.
        };

        PROPERTY(Hidden)
        float  X = 0.0f;

        PROPERTY(Hidden)
        float  Y = 0.0f;

        /** Seconds the current press has lasted, and zero while up. */
        PROPERTY(Hidden)
        float  HeldTime = 0.0f;

        PROPERTY(Hidden)
        uint32 Flags = 0;

        bool IsDown()     const { return (Flags & Flag_Down) != 0; }
        bool IsPressed()  const { return (Flags & Flag_Pressed) != 0; }
        bool IsReleased() const { return (Flags & Flag_Released) != 0; }
        bool IsHeld()     const { return (Flags & Flag_Held) != 0; }
        bool IsTapped()   const { return (Flags & Flag_Tapped) != 0; }
        bool IsDoubleTapped() const { return (Flags & Flag_DoubleTapped) != 0; }
        bool IsRepeated()     const { return (Flags & Flag_Repeated) != 0; }
        bool IsBuffered()     const { return (Flags & Flag_Buffered) != 0; }
    };

    // A player's replacement for one key binding of an authored action, saved in their user settings.
    REFLECT()
    struct RUNTIME_API SInputBindingOverride
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName Action;

        // Index into the action's authored Bindings.
        PROPERTY(Hidden)
        int32 BindingIndex = 0;

        PROPERTY(Hidden)
        SKey Key;
    };

    // What a player chose for input in their settings menu, applied over the project's authored actions.
    REFLECT()
    struct RUNTIME_API SPlayerInputSettings
    {
        GENERATED_BODY()

        // Scales mouse look, the MouseX and MouseY sources.
        PROPERTY(Hidden)
        float MouseSensitivity = 1.0f;

        // Scales gamepad look, the right stick sources.
        PROPERTY(Hidden)
        float GamepadLookSensitivity = 1.0f;

        PROPERTY(Hidden)
        bool bInvertMouseY = false;

        // Flips the right stick's Y only, so movement on the left stick is untouched.
        PROPERTY(Hidden)
        bool bInvertGamepadLookY = false;

        // Reads captured mouse motion straight from the device, skipping the desktop's pointer acceleration.
        PROPERTY(Hidden)
        bool bRawMouseInput = true;

        // Digital actions the player turned from hold into toggle, such as crouch or aim.
        PROPERTY(Hidden)
        TVector<FName> ToggleActions;

        PROPERTY(Hidden)
        TVector<SInputBindingOverride> BindingOverrides;
    };
}
