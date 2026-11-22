using System;
using System.Runtime.InteropServices;

namespace Lumina;

// Blittable integer-vector mirrors that match the native TVec layout byte-for-byte.

[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FUIntVector2")]
public struct FUIntVector2 : IEquatable<FUIntVector2>
{
    public uint X;
    public uint Y;

    public FUIntVector2(uint X, uint Y) { this.X = X; this.Y = Y; }
    public FUIntVector2(uint Value) { X = Value; Y = Value; }

    public static FUIntVector2 Zero => new(0u, 0u);

    public bool Equals(FUIntVector2 Other) => X == Other.X && Y == Other.Y;
    public override bool Equals(object? Obj) => Obj is FUIntVector2 V && Equals(V);
    public override int GetHashCode() => HashCode.Combine(X, Y);
    public override string ToString() => $"({X}, {Y})";
}

[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FUIntVector3")]
public struct FUIntVector3 : IEquatable<FUIntVector3>
{
    public uint X;
    public uint Y;
    public uint Z;

    public FUIntVector3(uint X, uint Y, uint Z) { this.X = X; this.Y = Y; this.Z = Z; }
    public FUIntVector3(uint Value) { X = Value; Y = Value; Z = Value; }
    public FUIntVector3(uint X, uint Y) { this.X = X; this.Y = Y; Z = 0u; }

    public static FUIntVector3 Zero => new(0u, 0u, 0u);

    public bool Equals(FUIntVector3 Other) => X == Other.X && Y == Other.Y && Z == Other.Z;
    public override bool Equals(object? Obj) => Obj is FUIntVector3 V && Equals(V);
    public override int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override string ToString() => $"({X}, {Y}, {Z})";
}

[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FIntVector2")]
public struct FIntVector2 : IEquatable<FIntVector2>
{
    public int X;
    public int Y;

    public FIntVector2(int X, int Y) { this.X = X; this.Y = Y; }
    public FIntVector2(int Value) { X = Value; Y = Value; }

    public static FIntVector2 Zero => new(0, 0);

    public bool Equals(FIntVector2 Other) => X == Other.X && Y == Other.Y;
    public override bool Equals(object? Obj) => Obj is FIntVector2 V && Equals(V);
    public override int GetHashCode() => HashCode.Combine(X, Y);
    public override string ToString() => $"({X}, {Y})";
}

[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FIntVector3")]
public struct FIntVector3 : IEquatable<FIntVector3>
{
    public int X;
    public int Y;
    public int Z;

    public FIntVector3(int X, int Y, int Z) { this.X = X; this.Y = Y; this.Z = Z; }
    public FIntVector3(int Value) { X = Value; Y = Value; Z = Value; }

    public static FIntVector3 Zero => new(0, 0, 0);

    public bool Equals(FIntVector3 Other) => X == Other.X && Y == Other.Y && Z == Other.Z;
    public override bool Equals(object? Obj) => Obj is FIntVector3 V && Equals(V);
    public override int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override string ToString() => $"({X}, {Y}, {Z})";
}
