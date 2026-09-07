using System;
using System.Runtime.InteropServices;

namespace LuminaSharp.Rendering;

// RHI handles. Each mirrors the engine's THandle<T> = { uint64 Handle } byte-for-byte, so it crosses the
// native boundary as a single 8-byte value with zero marshalling. Invalid == 0. Read-only value types:
// copy them freely; they are just a number.

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FPipelineH")]
public readonly struct FPipelineH : IEquatable<FPipelineH>
{
    public readonly ulong Handle;
    public FPipelineH(ulong Handle) => this.Handle = Handle;
    public bool IsValid => Handle != 0;
    public static FPipelineH Invalid => default;
    public bool Equals(FPipelineH Other) => Handle == Other.Handle;
    public override bool Equals(object? Obj) => Obj is FPipelineH H && Equals(H);
    public override int GetHashCode() => Handle.GetHashCode();
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FTextureH")]
public readonly struct FTextureH : IEquatable<FTextureH>
{
    public readonly ulong Handle;
    public FTextureH(ulong Handle) => this.Handle = Handle;
    public bool IsValid => Handle != 0;
    public static FTextureH Invalid => default;
    public bool Equals(FTextureH Other) => Handle == Other.Handle;
    public override bool Equals(object? Obj) => Obj is FTextureH H && Equals(H);
    public override int GetHashCode() => Handle.GetHashCode();
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FTextureHeapH")]
public readonly struct FTextureHeapH : IEquatable<FTextureHeapH>
{
    public readonly ulong Handle;
    public FTextureHeapH(ulong Handle) => this.Handle = Handle;
    public bool IsValid => Handle != 0;
    public static FTextureHeapH Invalid => default;
    public bool Equals(FTextureHeapH Other) => Handle == Other.Handle;
    public override bool Equals(object? Obj) => Obj is FTextureHeapH H && Equals(H);
    public override int GetHashCode() => Handle.GetHashCode();
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FSemaphoreH")]
public readonly struct FSemaphoreH : IEquatable<FSemaphoreH>
{
    public readonly ulong Handle;
    public FSemaphoreH(ulong Handle) => this.Handle = Handle;
    public bool IsValid => Handle != 0;
    public static FSemaphoreH Invalid => default;
    public bool Equals(FSemaphoreH Other) => Handle == Other.Handle;
    public override bool Equals(object? Obj) => Obj is FSemaphoreH H && Equals(H);
    public override int GetHashCode() => Handle.GetHashCode();
}


[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FCmdListH")]
public readonly struct FCmdListH : IEquatable<FCmdListH>
{
    public readonly ulong Handle;
    public FCmdListH(ulong Handle) => this.Handle = Handle;
    public bool IsValid => Handle != 0;
    public static FCmdListH Invalid => default;
    public bool Equals(FCmdListH Other) => Handle == Other.Handle;
    public override bool Equals(object? Obj) => Obj is FCmdListH H && Equals(H);
    public override int GetHashCode() => Handle.GetHashCode();
}

/// <summary>
/// A device-addressable GPU memory pointer (the engine's <c>GPUPtr</c> = uint64). Allocate with
/// <see cref="RHI.Malloc"/>, which returns an <see cref="FGPUAllocation"/> carrying this pointer, its
/// CPU mapping and its size. Supports byte-offset arithmetic. Zero is the null/invalid pointer.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::GPUPtr")]
public readonly struct GPUPtr : IEquatable<GPUPtr>
{
    public readonly ulong Value;
    public GPUPtr(ulong Value) => this.Value = Value;

    public bool IsValid => Value != 0;
    public static GPUPtr Null => default;

    public static GPUPtr operator +(GPUPtr Ptr, ulong Offset) => new(Ptr.Value + Offset);
    public static GPUPtr operator +(GPUPtr Ptr, long Offset) => new((ulong)((long)Ptr.Value + Offset));
    public static implicit operator ulong(GPUPtr Ptr) => Ptr.Value;
    public static explicit operator GPUPtr(ulong Value) => new(Value);

    public bool Equals(GPUPtr Other) => Value == Other.Value;
    public override bool Equals(object? Obj) => Obj is GPUPtr P && Equals(P);
    public override int GetHashCode() => Value.GetHashCode();
    public override string ToString() => $"GPUPtr(0x{Value:X})";
}
