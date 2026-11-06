using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// An action name a [Property] picks from the project's input actions, fed from OnAction before OnUpdate.
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

    // The authored action this binding listens to, picked in the inspector and assignable at runtime.
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
            // Dropped so a rename cannot leave a press latched and fire a release for an action no longer watched.
            bIdResolved = false;
            State = default;
            bPrimed = false;
        }
    }

    private string ActionName = string.Empty;

    // This frame's raw state, for anything the typed members do not expose.
    public Lumina.SInputActionState State { get; private set; }

    // True when the name is an action in the project's input settings.
    public bool IsBound => !string.IsNullOrEmpty(ActionName) && Lumina.CInputLibrary.FindActionIndex(ActionName) >= 0;

    // The X channel of an Axis2D action.
    public float Value => State.X;

    // Y is 0 for any action that is not Axis2D.
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

    // Called from CEntityScript.OnAction every frame the bound action changes, never from game code.
    internal void Apply(in Lumina.SInputActionState Next)
    {
        // The first delivery is its own previous state, so a key held at bind time does not read as a fresh press.
        Lumina.SInputActionState Previous = bPrimed ? State : Next;
        bPrimed = true;
        State = Next;

        Raise(in Previous);
    }

    // Fires this binding's events for the transition from Previous to the state just applied.
    private protected abstract void Raise(in Lumina.SInputActionState Previous);

}

// A digital binding, which also works on an axis action by being down whenever its value leaves the dead zone.
public sealed class SInputAction : SInputBinding
{
    public SInputAction()
    {
    }

    public SInputAction(string Name)
        : base(Name)
    {
    }

    public event Action? Pressed;

    public event Action? Released;

    // Raised with the seconds held every frame the action has been down for its HoldTime, which defaults to 0.
    public event Action<float>? Held;

    // Raised on the frame a press shorter than the action's TapTime is released.
    public event Action? Tapped;

    // Raised on a second press within the action's DoubleTapTime.
    public event Action? DoubleTapped;

    // Raised on the press and again each RepeatInterval while held, as a menu scrolls.
    public event Action? Repeated;

    public bool IsDown => State.IsDown;

    public bool WasPressed => State.IsPressed;

    public bool WasReleased => State.IsReleased;

    public bool IsHeld => State.IsHeld;

    // Seconds the current press has lasted, 0 while up.
    public float HeldTime => State.HeldTime;

    private protected override void Raise(in Lumina.SInputActionState Previous)
    {
        Lumina.SInputActionState Current = State;

        // The last delivered state catches a release the entity missed while not receiving input.
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
        if (Current.IsDoubleTapped)
        {
            DoubleTapped?.Invoke();
        }
        if (Current.IsRepeated)
        {
            Repeated?.Invoke();
        }
    }
}

// An analog binding, read through Value in OnUpdate or through Changed to react only when it moves.
public sealed class SInputAxis : SInputBinding
{
    public SInputAxis()
    {
    }

    public SInputAxis(string Name)
        : base(Name)
    {
    }

    // Raised when the value differs from last frame, including the frame it returns to zero.
    public event Action<float>? Changed;

    public event Action<Lumina.FVector2>? Changed2D;

    public bool IsMoving => State.X != 0.0f || State.Y != 0.0f;

    private protected override void Raise(in Lumina.SInputActionState Previous)
    {
        Lumina.SInputActionState Current = State;
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
