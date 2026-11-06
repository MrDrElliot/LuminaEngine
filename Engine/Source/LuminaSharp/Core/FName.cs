using System;
using LuminaSharp;

namespace Lumina;

// By-value mirror of the native POD FName, so equality and hashing never cross to native; LayoutValidator asserts its size.
[NativeLayout("FName")]
public struct FName : IEquatable<FName>
{
    // Mirrors Lumina::FName in Containers/Name.h; Index is a native name table slot, not a hash, and not stable between runs.
    private uint Index;
    private uint Number;

    // Matches NAME_None, and its wire form is the empty string.
    public static FName None => default;

    public FName(string Value)
    {
        this = FromString(Value);
    }

    public static unsafe FName FromString(string Value)
    {
        FName Result = default;
        Native.NameFromString(Value ?? "", (IntPtr)(&Result));
        return Result;
    }

    public readonly bool IsNone => Index == 0 && Number == 0;

    public override readonly unsafe string ToString()
    {
        FName Self = this;
        return Native.NameToString((IntPtr)(&Self));
    }

    // Identity is the index pair, since the native table already folds in case-insensitivity.
    public readonly bool Equals(FName Other) => Index == Other.Index && Number == Other.Number;

    public override readonly bool Equals(object? Other) => Other is FName Value && Equals(Value);

    public override readonly int GetHashCode() => HashCode.Combine(Index, Number);

    public static bool operator ==(FName Left, FName Right) => Left.Equals(Right);

    public static bool operator !=(FName Left, FName Right) => !Left.Equals(Right);

    public static implicit operator FName(string Value) => FromString(Value);

    public static implicit operator string(FName Value) => Value.ToString();
}
