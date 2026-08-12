using System;
using LuminaSharp;

namespace Lumina;

/// <summary>
/// C# mirror of <c>Lumina::FString</c> (<c>eastl::basic_string&lt;char&gt;</c>), as a HANDLE to one rather than
/// a copy of one.
///
/// Why a handle and not a 24-byte layout mirror. An eastl string owns a heap buffer, so a struct that mirrored
/// its bytes would copy the POINTER on every managed assignment -- two live strings aliasing one buffer, which
/// is the exact hazard that used to force a bespoke view type for lists of strings. A handle cannot do that:
/// it names a slot, and every read and write goes through the native accessors that know the layout
/// (<see cref="NativeMarshal.ReadString"/> decodes the SSO/heap union in place, <c>Native.StringAssign</c>
/// assigns through eastl).
///
/// Two shapes, and the difference is only where the characters are:
/// <list type="bullet">
/// <item>BOUND -- names a live native slot (an element of a <see cref="TVector{T}"/>, say). Reads decode that
/// slot on demand, so the value is always current. Like every other view here, a mutation of the owning
/// container invalidates it.</item>
/// <item>LITERAL -- built from a managed string, holding nothing native. This is what you write INTO a slot;
/// the implicit conversion means <c>Names.Add("first")</c> just works.</item>
/// </list>
/// </summary>
public readonly struct FString : IEquatable<FString>
{
    private readonly nint Slot;        // native FString address, or 0 for a literal
    private readonly string? Literal;  // the value when unbound

    /// <summary>A literal, to be assigned into native storage.</summary>
    public FString(string Value)
    {
        Slot = 0;
        Literal = Value ?? "";
    }

    /// <summary>Binds to a live native FString. Internal because handing out an arbitrary address as a string
    /// is exactly the mistake this type exists to prevent; the container views construct these.</summary>
    internal FString(nint Address)
    {
        Slot = Address;
        Literal = null;
    }

    /// <summary>The native slot this names, or 0 when it is a literal.</summary>
    internal nint Address => Slot;

    /// <summary>True when this names live native storage rather than carrying a managed value.</summary>
    public bool IsBound => Slot != 0;

    /// <summary>The characters, decoded from native storage on each read for a bound handle.</summary>
    public override string ToString() => Slot != 0 ? NativeMarshal.ReadString(Slot) : Literal ?? "";

    public int Length => ToString().Length;

    public bool IsEmpty => Length == 0;

    public static implicit operator string(FString Value) => Value.ToString();

    public static implicit operator FString(string Value) => new FString(Value);

    // Value equality on the characters, so a bound handle and a literal holding the same text compare equal.
    // Comparing slots instead would make `Names[0] == "first"` always false, which is the only comparison
    // anyone actually writes.
    public bool Equals(FString Other) => string.Equals(ToString(), Other.ToString(), StringComparison.Ordinal);

    public override bool Equals(object? Other) => Other is FString Value && Equals(Value);

    public override int GetHashCode() => ToString().GetHashCode(StringComparison.Ordinal);

    public static bool operator ==(FString Left, FString Right) => Left.Equals(Right);

    public static bool operator !=(FString Left, FString Right) => !Left.Equals(Right);
}
