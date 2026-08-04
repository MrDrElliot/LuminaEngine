using System.Runtime.InteropServices;

namespace Lumina;

/// <summary>
/// One input action's evaluated state for the current frame. Blittable mirror of the native
/// <c>FInputActionState</c>; the script layer reads a whole array of these through a pointer rather than
/// crossing into native per action. Read it through <see cref="LuminaSharp.SInputAction"/> /
/// <see cref="LuminaSharp.SInputAxis"/> rather than directly.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FInputActionState")]
public struct FInputActionState
{
    /// <summary>The action's value (its X channel for an Axis2D action). 1/0 for a digital action.</summary>
    public float X;

    /// <summary>The Y channel of an Axis2D action; 0 otherwise.</summary>
    public float Y;

    /// <summary>Seconds the current press has lasted; 0 while the action is up.</summary>
    public float HeldTime;

    /// <summary>Bit flags; use the properties rather than reading this directly.</summary>
    public uint Flags;

    public bool IsDown     => (Flags & 1u) != 0;
    public bool IsPressed  => (Flags & 2u) != 0;
    public bool IsReleased => (Flags & 4u) != 0;
    public bool IsHeld     => (Flags & 8u) != 0;
    public bool IsTapped   => (Flags & 16u) != 0;
}
