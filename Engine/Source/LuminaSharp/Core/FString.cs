using System;
using LuminaSharp;

namespace Lumina;

// Handle to a native FString, either bound to a live slot or a literal to assign into one, so copies never alias a heap buffer.
public readonly struct FString : IEquatable<FString>
{
    private readonly nint Slot;        // native FString address, or 0 for a literal
    private readonly string? Literal;  // the value when unbound

    public FString(string Value)
    {
        Slot = 0;
        Literal = Value ?? "";
    }

    // Internal so an arbitrary address cannot pass as a string; the container views construct these.
    internal FString(nint Address)
    {
        Slot = Address;
        Literal = null;
    }

    // Zero for a literal.
    internal nint Address => Slot;

    public bool IsBound => Slot != 0;

    public override string ToString() => Slot != 0 ? NativeMarshal.ReadString(Slot) : Literal ?? "";

    public int Length => ToString().Length;

    public bool IsEmpty => Length == 0;

    public static implicit operator string(FString Value) => Value.ToString();

    public static implicit operator FString(string Value) => new FString(Value);

    // Compares characters, not slots, so a bound handle equals a literal holding the same text.
    public bool Equals(FString Other) => string.Equals(ToString(), Other.ToString(), StringComparison.Ordinal);

    public override bool Equals(object? Other) => Other is FString Value && Equals(Value);

    public override int GetHashCode() => ToString().GetHashCode(StringComparison.Ordinal);

    public static bool operator ==(FString Left, FString Right) => Left.Equals(Right);

    public static bool operator !=(FString Left, FString Right) => !Left.Equals(Right);
}
