using System;
using LuminaSharp;

namespace Lumina;

/// <summary>
/// C# mirror of <c>Lumina::FName</c>: an interned, case-insensitively pooled name, stored as a table index
/// plus a number rather than as text.
///
/// Unlike <see cref="FString"/> this is a real by-value mirror, not a handle, because the native type is POD --
/// there is no heap buffer to alias, so copying the bytes IS copying the name. That is what lets an FName
/// member be read and written in place at the property's offset like an <c>FVector3</c>, and an FName element
/// sit in a <see cref="TVector{T}"/> with no marshalling at all.
///
/// The two conversions that do need the name table cross to native: text in, text out. Everything else --
/// equality, hashing, IsNone -- is answered from the id, which is the point of interning.
///
/// <para>The size is asserted against the native type at bootstrap by LayoutValidator (see the
/// <c>LE_REGISTER_LAYOUT("FName", FName)</c> in CSharpLayoutChecks.cpp), so a field added on either side
/// fails loudly instead of silently shifting every element of an FName container.</para>
/// </summary>
[NativeLayout("FName")]
public struct FName : IEquatable<FName>
{
    // Mirrors Lumina::FName's members in declaration order (Containers/Name.h): uint32 Index, uint32 Number.
    // Index is a dense slot in the native name table, NOT a hash, and not stable between runs.
    private uint Index;
    private uint Number;

    /// <summary>The empty name, matching <c>NAME_None</c>. Its wire form is the empty string.</summary>
    public static FName None => default;

    public FName(string Value)
    {
        this = FromString(Value);
    }

    /// <summary>Interns <paramref name="Value"/> and returns the resulting name.</summary>
    public static unsafe FName FromString(string Value)
    {
        FName Result = default;
        Native.NameFromString(Value ?? "", (IntPtr)(&Result));
        return Result;
    }

    /// <summary>True for <see cref="None"/>. Answered from the index, with no crossing.</summary>
    public readonly bool IsNone => Index == 0 && Number == 0;

    /// <summary>Resolves the interned id back to text.</summary>
    public override readonly unsafe string ToString()
    {
        FName Self = this;
        return Native.NameToString((IntPtr)(&Self));
    }

    // Identity is the index pair, which is what interning buys: no string compare, and case-insensitivity is
    // already folded in by the native table.
    public readonly bool Equals(FName Other) => Index == Other.Index && Number == Other.Number;

    public override readonly bool Equals(object? Other) => Other is FName Value && Equals(Value);

    public override readonly int GetHashCode() => HashCode.Combine(Index, Number);

    public static bool operator ==(FName Left, FName Right) => Left.Equals(Right);

    public static bool operator !=(FName Left, FName Right) => !Left.Equals(Right);

    public static implicit operator FName(string Value) => FromString(Value);

    public static implicit operator string(FName Value) => Value.ToString();
}
