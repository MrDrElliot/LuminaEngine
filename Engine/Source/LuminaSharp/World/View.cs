using System;

namespace LuminaSharp;

/// Exclude filter for a View, mirroring <c>ECS::TExclude&lt;...&gt;</c>; build one with <c>Exclude.Of&lt;...&gt;()</c>.
public readonly struct Exclude
{
    internal readonly IntPtr Token0;
    internal readonly IntPtr Token1;
    internal readonly IntPtr Token2;
    internal readonly int Count;

    internal Exclude(IntPtr Token0, IntPtr Token1, IntPtr Token2, int Count)
    {
        this.Token0 = Token0;
        this.Token1 = Token1;
        this.Token2 = Token2;
        this.Count = Count;
    }

    /// <summary>An empty exclude set (the default for an unfiltered view).</summary>
    public static readonly Exclude None = new(IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0);

    /// Excludes entities carrying T1.
    public static Exclude Of<T1>()
        where T1 : NativeStruct
    {
        return new Exclude(ComponentOps<T1>.Token, IntPtr.Zero, IntPtr.Zero, 1);
    }

    /// Excludes entities carrying either of T1, T2.
    public static Exclude Of<T1, T2>()
        where T1 : NativeStruct
        where T2 : NativeStruct
    {
        return new Exclude(ComponentOps<T1>.Token, ComponentOps<T2>.Token, IntPtr.Zero, 2);
    }

    /// Excludes entities carrying any of T1, T2, T3.
    public static Exclude Of<T1, T2, T3>()
        where T1 : NativeStruct
        where T2 : NativeStruct
        where T3 : NativeStruct
    {
        return new Exclude(ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, 3);
    }
}

/// <summary>
/// Builds one fresh component wrapper per type for a single View iteration, via its generated
/// <c>(IntPtr)</c> ctor (handle 0). The View rebinds the wrapper's internal handle field each step, so
/// there is NO per-element allocation -- one wrapper services the whole iteration. A fresh wrapper per
/// Each/foreach call keeps nested and parallel iterations independent.
/// </summary>
internal static class ViewWrapper<T> where T : NativeStruct
{
    public static T New()
    {
        return Wrapper<T>.Create(IntPtr.Zero) ?? throw new InvalidOperationException(
            $"{typeof(T).Name} has no (IntPtr) ctor; cannot build a reusable View wrapper.");
    }
}
