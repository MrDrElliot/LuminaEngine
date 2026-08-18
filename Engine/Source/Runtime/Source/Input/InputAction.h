#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/Key.h"
#include "InputAction.generated.h"

namespace Lumina
{
    // What an action produces. Digital is on/off; the axis types read a continuous value out of the same
    // binding list, so one action can be queried either way (a digital action still reports 1/0 as a value).
    REFLECT()
    enum class EInputActionType : uint8
    {
        Digital,
        Axis1D,
        Axis2D,
    };

    // Where a binding's value comes from. Key is the SKey (its Scale while held); the rest are continuous
    // device sources whose per-frame motion is multiplied by Scale, and which ignore the SKey entirely.
    REFLECT()
    enum class EInputAxisSource : uint8
    {
        Key,
        MouseX,
        MouseY,
        MouseWheel,
    };

    // Which channel of an Axis2D action a binding feeds. Ignored by Digital and Axis1D, which only ever
    // use X.
    REFLECT()
    enum class EInputAxisChannel : uint8
    {
        X,
        Y,
    };

    // One physical binding for an action: an SKey (keyboard/mouse, with its own Ctrl/Shift/Alt chord) or a
    // continuous device source, plus the scale it contributes.
    REFLECT()
    struct RUNTIME_API SInputActionBinding
    {
        GENERATED_BODY()

        SInputActionBinding() = default;
        explicit SInputActionBinding(const SKey& InKey, float InScale = 1.0f)
            : Key(InKey), Scale(InScale) {}

        PROPERTY(Editable)
        SKey Key;

        // Value contributed while Key is held (e.g. +1 / -1), or the multiplier applied to a continuous
        // source's per-frame motion. A digital action only cares about the sign being non-zero.
        PROPERTY(Editable)
        float Scale = 1.0f;

        // Key reads the SKey above; the mouse sources read this frame's motion and ignore the SKey.
        PROPERTY(Editable)
        EInputAxisSource Source = EInputAxisSource::Key;

        // Axis2D only: which channel this binding drives. W/S on Y and A/D on X make a movement stick.
        PROPERTY(Editable)
        EInputAxisChannel Channel = EInputAxisChannel::X;
    };

    // A named gameplay input: the bindings that feed it plus the shaping applied to their sum. Authored on
    // CInputSettings, evaluated once per frame per input context by FInputActionMap.
    //
    // NoCSharp: this is the authored DEFINITION, which the script layer never reads. The name belongs to the
    // script-facing binding (LuminaSharp.SInputAction) instead; emitting a wrapper here would put two
    // different SInputAction types in scope of every script and make the name ambiguous.
    REFLECT(NoCSharp)
    struct RUNTIME_API SInputAction
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        // Digital = on/off; Axis1D = the summed value; Axis2D = the summed value per channel.
        PROPERTY(Editable)
        EInputActionType Type = EInputActionType::Digital;

        // Keep firing while the active context is in EInputMode::UI (pause / save hotkeys).
        PROPERTY(Editable)
        bool bRunsInUI = false;

        // Input below this magnitude reads as zero, and the remainder is rescaled so full deflection still
        // reaches the original range. Applied radially for Axis2D. 0 disables it.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 0.99f)
        float DeadZone = 0.0f;

        // Multiplies the shaped value. The usual home for mouse-look sensitivity.
        PROPERTY(Editable, ClampMin = 0.0f)
        float Sensitivity = 1.0f;

        PROPERTY(Editable)
        bool bInvert = false;

        // Seconds the action must stay down before it counts as held. 0 means "held from the first frame
        // it is down", which is the every-frame-while-pressed behaviour.
        PROPERTY(Editable, ClampMin = 0.0f)
        float HoldTime = 0.0f;

        // A press released within this many seconds also reports a tap on the release frame.
        PROPERTY(Editable, ClampMin = 0.0f)
        float TapTime = 0.2f;

        PROPERTY(Editable)
        TVector<SInputActionBinding> Bindings;

        //~ Migration: actions authored before EInputActionType existed carry bAxis instead. Not Editable, so
        //  it never shows in the Input settings, but still round-trips through the config file until
        //  FInputActionMap::RebuildFromSettings folds it into Type.
        PROPERTY()
        bool bAxis = false;

        bool IsAxis() const { return Type != EInputActionType::Digital; }
    };

    // A named layer of actions, pushed onto a world's input stack to change what input means right now:
    // open the pause menu and only the menu's actions should fire. Authored on CInputSettings next to the
    // actions themselves, so what a layer contains is data rather than code.
    REFLECT()
    struct RUNTIME_API SInputMappingContext
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        /** The actions this layer allows. An action listed here fires while the layer is on the stack. */
        PROPERTY(Editable)
        TVector<FName> Actions;

        // The point of a menu layer: an action NOT listed here stops at this layer instead of reaching
        // gameplay underneath. Leave it false for a layer that only adds actions.
        PROPERTY(Editable)
        bool bBlockLower = true;
    };

    // A reference to an authored action, resolved once instead of hashed per query. The NAME is what it
    // really holds: a settings rebuild moves indices, and re-resolving from the name means gameplay code
    // survives a rebind without knowing one happened. Serial 0 means never resolved (the map's first
    // rebuild sets it to 1).
    struct FInputActionHandle
    {
        FInputActionHandle() = default;
        explicit FInputActionHandle(FName InName) : Name(InName) {}

        FName GetName() const { return Name; }

        // Cleared by assigning a new name, so a rebound handle cannot keep answering for the old action.
        void SetName(FName InName)
        {
            Name = InName;
            CachedIndex = INDEX_NONE;
            CachedSerial = 0;
        }

        bool IsSet() const { return !Name.IsNone(); }

    private:

        friend class FInputActionMap;

        FName          Name;
        mutable int32  CachedIndex = INDEX_NONE;
        mutable uint32 CachedSerial = 0;
    };

    // Per-frame evaluated state of one action within one input context. Plain data, mirrored byte for byte by
    // LuminaSharp.InputActionState: the script layer reads the whole array through a pointer instead of
    // crossing into native per action.
    struct FInputActionState
    {
        enum EFlags : uint32
        {
            Flag_Down     = 1u << 0,  ///< Down this frame.
            Flag_Pressed  = 1u << 1,  ///< Went down this frame.
            Flag_Released = 1u << 2,  ///< Came up this frame.
            Flag_Held     = 1u << 3,  ///< Down and past the action's HoldTime.
            Flag_Tapped   = 1u << 4,  ///< Released this frame after less than TapTime down.
        };

        float  X = 0.0f;
        float  Y = 0.0f;
        float  HeldTime = 0.0f;   ///< Seconds the current press has lasted; 0 while up.
        uint32 Flags = 0;

        bool IsDown()     const { return (Flags & Flag_Down) != 0; }
        bool IsPressed()  const { return (Flags & Flag_Pressed) != 0; }
        bool IsReleased() const { return (Flags & Flag_Released) != 0; }
        bool IsHeld()     const { return (Flags & Flag_Held) != 0; }
        bool IsTapped()   const { return (Flags & Flag_Tapped) != 0; }
    };
}
