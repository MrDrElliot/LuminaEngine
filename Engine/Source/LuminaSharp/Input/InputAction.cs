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
/// be receiving input, exactly like the polling queries and <c>OnInput</c>. Bindings are fed from
/// <c>OnAction</c> before OnUpdate, so an event handler sees the same state a poll in OnUpdate would.
/// </remarks>
public abstract partial class SInputBinding
{
    // The interned form of ActionName, so a dispatch compares ids instead of crossing to resolve text.
    private Lumina.FName ActionId;
    private bool bIdResolved;

    // False until the first Apply, so the first delivery has no prior state to compare against.
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
            // Drop the old action's state so a rename can't leave a press latched (which would fire a
            // release for an action this binding no longer watches).
            bIdResolved = false;
            State = default;
            bPrimed = false;
        }
    }

    private string ActionName = string.Empty;

    /// <summary>This frame's raw state, for anything the typed members don't expose.</summary>
    public Lumina.FInputActionState State { get; private set; }

    /// <summary>True when the name is an action that exists in the project's input settings.</summary>
    public bool IsBound => !string.IsNullOrEmpty(ActionName) && Lumina.CInputLibrary.FindActionIndex(ActionName) >= 0;

    /// <summary>The action's value this frame (X channel for an Axis2D action).</summary>
    public float Value => State.X;

    /// <summary>Both channels of an Axis2D action; Y is 0 for other action types.</summary>
    public Lumina.FVector2 Value2D => new Lumina.FVector2(State.X, State.Y);

    public override string ToString() => $"{GetType().Name}('{ActionName}')";

    internal bool Listens(Lumina.FName Action)
    {
        if (!bIdResolved)
        {
            ActionId = string.IsNullOrEmpty(ActionName) ? Lumina.FName.None : new Lumina.FName(ActionName);
            bIdResolved = true;
        }
        return !ActionId.IsNone && ActionId == Action;
    }

    // Applies the action's state and raises whatever the concrete binding raises. Called from
    // EntityScript.OnAction every frame the bound action changes; never call it from game code.
    internal void Apply(in Lumina.FInputActionState Next)
    {
        // The first delivery has no previous frame to compare against: hand Raise the incoming state as the
        // previous one so a key already held at bind time doesn't read as a press that just happened.
        Lumina.FInputActionState Previous = bPrimed ? State : Next;
        bPrimed = true;
        State = Next;

        Raise(in Previous);
    }

    // Fires this binding's events for the transition from Previous to the state just applied.
    private protected abstract void Raise(in Lumina.FInputActionState Previous);

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

    private protected override void Raise(in Lumina.FInputActionState Previous)
    {
        Lumina.FInputActionState Current = State;

        // Compared against the last delivered state as well as the engine's flag: a release the entity was
        // not receiving input for would otherwise be missed and leave the press latched.
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

    private protected override void Raise(in Lumina.FInputActionState Previous)
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
