using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Base for the two script-facing input bindings, <see cref="SInputAction"/> and <see cref="SInputAxis"/>.
/// A binding is just a name plus the state the engine evaluated for it this frame; declare one as a
/// <c>[Property]</c> field and the inspector shows a dropdown of the project's authored input actions
/// (Settings &gt; Engine &gt; Input), so the key a script reacts to is data, not a hardcoded string.
/// </summary>
/// <remarks>
/// The owning entity needs an <c>SInputComponent</c> (call <c>EnableInput()</c> in <c>OnReady</c>) and must
/// be receiving input, exactly like the polling queries and <c>OnInput</c>. Bindings are polled once per
/// frame before OnUpdate, so an event handler sees the same state a poll in OnUpdate would.
/// </remarks>
public abstract partial class SInputBinding
{
    private int Index = -1;

    // The action-table serial Index was resolved against. Native serials start at 1, so 0 means "never
    // resolved"; a settings change bumps the serial and forces exactly one re-resolve per binding.
    private uint ResolvedSerial;

    // False until the first Poll, so the first frame has no prior state to raise edges against.
    private bool bPrimed;

    protected SInputBinding()
    {
    }

    protected SInputBinding(string Name)
    {
        this.Name = Name ?? string.Empty;
    }

    /// <summary>The authored action this binding listens to. Picked in the inspector; assignable at runtime.</summary>
    public string Name
    {
        get => ActionName;
        set
        {
            if (ActionName == value)
            {
                return;
            }
            ActionName = value ?? string.Empty;
            // Re-resolve on the next poll, and drop the old action's state so a rename can't leave a
            // press latched (which would fire a release for an action this binding no longer watches).
            ResolvedSerial = 0;
            Index = -1;
            State = default;
            bPrimed = false;
        }
    }

    private string ActionName = string.Empty;

    /// <summary>This frame's raw state, for anything the typed members don't expose.</summary>
    public Lumina.FInputActionState State { get; private set; }

    /// <summary>True once the name has resolved to an action that exists in the project's input settings.</summary>
    public bool IsBound => Index >= 0;

    /// <summary>The action's value this frame (X channel for an Axis2D action).</summary>
    public float Value => State.X;

    /// <summary>Both channels of an Axis2D action; Y is 0 for other action types.</summary>
    public Lumina.FVector2 Value2D => new Lumina.FVector2(State.X, State.Y);

    public override string ToString() => $"{GetType().Name}('{ActionName}')";

    // Applies this frame's state and raises whatever the concrete binding raises. Called by the runtime
    // for every binding on a script whose entity is receiving input; never call it from game code.
    internal unsafe void Poll(Lumina.FInputActionState* States, int Count, uint Serial, float DeltaTime)
    {
        if (ResolvedSerial != Serial)
        {
            Index = string.IsNullOrEmpty(ActionName) ? -1 : FindActionIndex(ActionName);
            ResolvedSerial = Serial;
        }

        // An unresolved or out-of-range index reads as a zeroed state rather than skipping the update, so
        // an action deleted from the settings mid-session still delivers its release edge.
        Lumina.FInputActionState Next = (Index >= 0 && Index < Count) ? States[Index] : default;

        // The first poll has no previous frame to compare against: hand Raise the incoming state as the
        // previous one so a key already held at bind time doesn't read as a press that just happened.
        Lumina.FInputActionState Previous = bPrimed ? State : Next;
        bPrimed = true;
        State = Next;

        Raise(in Previous, DeltaTime);
    }

    // Fires this binding's events for the transition from Previous to the state just applied.
    private protected abstract void Raise(in Lumina.FInputActionState Previous, float DeltaTime);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Input_FindActionIndex")]
    private static partial int FindActionIndex(string Name);
}

/// <summary>
/// A digital input binding: subscribe to <see cref="Pressed"/> / <see cref="Released"/> instead of testing
/// a string every frame.
/// <code>
/// [Property] public SInputAction Interact = new("Interact");
///
/// public override void OnReady()
/// {
///     EnableInput();
///     Interact.Pressed += () =&gt; Use();
/// }
/// </code>
/// Works against an axis action too: it is down whenever the action's value leaves the dead zone.
/// </summary>
public sealed class SInputAction : SInputBinding
{
    public SInputAction()
    {
    }

    public SInputAction(string Name)
        : base(Name)
    {
    }

    /// <summary>Raised on the frame the action goes down.</summary>
    public event Action? Pressed;

    /// <summary>Raised on the frame the action comes up.</summary>
    public event Action? Released;

    /// <summary>Raised every frame the action has been down for at least its authored HoldTime (0 by
    /// default, so this fires on every frame it is down). The argument is seconds held.</summary>
    public event Action<float>? Held;

    /// <summary>Raised on the frame a press shorter than the action's TapTime is released.</summary>
    public event Action? Tapped;

    /// <summary>True while the action is down.</summary>
    public bool IsDown => State.IsDown;

    /// <summary>True on the frame it went down.</summary>
    public bool WasPressed => State.IsPressed;

    /// <summary>True on the frame it came up.</summary>
    public bool WasReleased => State.IsReleased;

    /// <summary>True while down and past the authored HoldTime.</summary>
    public bool IsHeld => State.IsHeld;

    /// <summary>Seconds the current press has lasted; 0 while up.</summary>
    public float HeldTime => State.HeldTime;

    private protected override void Raise(in Lumina.FInputActionState Previous, float DeltaTime)
    {
        Lumina.FInputActionState Current = State;

        // Edges come from the transition as well as from the engine's own flag. They agree frame to frame;
        // the transition is what covers a frame the script was NOT polled on (the entity stopped receiving
        // input mid-press), which would otherwise swallow the release and leave the game latched.
        bool bDown = Current.IsDown;
        bool bWasDown = Previous.IsDown;

        if (Current.IsPressed || (bDown && !bWasDown))
        {
            Pressed?.Invoke();
        }
        if (Current.IsHeld)
        {
            Held?.Invoke(Current.HeldTime);
        }
        if (Current.IsReleased || (!bDown && bWasDown))
        {
            Released?.Invoke();
        }
        if (Current.IsTapped)
        {
            Tapped?.Invoke();
        }
    }
}

/// <summary>
/// An analog input binding: read <see cref="SInputBinding.Value"/> in OnUpdate, or subscribe to
/// <see cref="Changed"/> to react only when it moves.
/// <code>
/// [Property] public SInputAxis Move = new("MoveForward");
///
/// public override void OnUpdate(float Delta) =&gt; Walk(Move.Value * Delta);
/// </code>
/// An Axis2D action fills <see cref="SInputBinding.Value2D"/> as well.
/// </summary>
public sealed class SInputAxis : SInputBinding
{
    public SInputAxis()
    {
    }

    public SInputAxis(string Name)
        : base(Name)
    {
    }

    /// <summary>Raised when the value differs from last frame, including the frame it returns to zero.</summary>
    public event Action<float>? Changed;

    /// <summary>Raised when either channel of an Axis2D action differs from last frame.</summary>
    public event Action<Lumina.FVector2>? Changed2D;

    /// <summary>True while the axis is off zero.</summary>
    public bool IsMoving => State.X != 0.0f || State.Y != 0.0f;

    private protected override void Raise(in Lumina.FInputActionState Previous, float DeltaTime)
    {
        Lumina.FInputActionState Current = State;
        if (Current.X != Previous.X)
        {
            Changed?.Invoke(Current.X);
        }
        if (Current.X != Previous.X || Current.Y != Previous.Y)
        {
            Changed2D?.Invoke(new Lumina.FVector2(Current.X, Current.Y));
        }
    }
}
