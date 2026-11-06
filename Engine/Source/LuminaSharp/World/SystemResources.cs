using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using Lumina;

namespace LuminaSharp;

// One per world, read through a TRes<T> field and written through a TResMut<T> one: a C# struct, or the view of a reflected C++ FResource.
public interface IResource
{
}

internal static unsafe partial class ResourceNatives
{
    [NativeCall] public static partial IntPtr FindResourceType(string Name);
    [NativeCall] public static partial IntPtr GetOrEmplaceResource(ulong World, IntPtr Type);
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr FindResource(ulong World, IntPtr Type);
    [NativeCall] public static partial uint GetNamedAccessToken(string Name);
}

internal static class ResourceType<T> where T : struct, IResource
{
    public static readonly bool bNative = typeof(INativeStruct).IsAssignableFrom(typeof(T));
    public static readonly string Name = bNative ? NativeTypeName.Of(typeof(T)) : "res:" + typeof(T).FullName;

    private static IntPtr Native;

    public static IntPtr RequireNative()
    {
        if (Native == IntPtr.Zero)
        {
            IntPtr Found = ResourceNatives.FindResourceType(Name);
            if (Found == IntPtr.Zero)
            {
                throw new InvalidOperationException($"{typeof(T).Name} is not a reflected C++ resource; it has to derive ECS::FResource and be REFLECT()ed.");
            }
            Native = Found;
        }
        return Native;
    }
}

// Every world's managed resources, kept apart from the world wrapper since that is weakly cached and the store must not pin a reloading script context.
internal static class WorldResources
{
    private static readonly Dictionary<ulong, Dictionary<Type, object>> ByWorld = new();

    internal static object GetOrAdd(ulong World, Type Type)
    {
        lock (ByWorld)
        {
            if (!ByWorld.TryGetValue(World, out Dictionary<Type, object>? Resources))
            {
                Resources = new Dictionary<Type, object>();
                ByWorld.Add(World, Resources);
            }
            if (!Resources.TryGetValue(Type, out object? Resource))
            {
                Resource = Activator.CreateInstance(Type)!;
                Resources.Add(Type, Resource);
            }
            return Resource;
        }
    }

    internal static void RemoveForWorld(ulong World)
    {
        lock (ByWorld)
        {
            ByWorld.Remove(World);
        }
    }

    internal static void Clear()
    {
        lock (ByWorld)
        {
            ByWorld.Clear();
        }
    }
}

// The world's instance, created on the main thread at bind, and the access it declares. Value is an extension: generated per native view, ResourceAccess for a C# struct.
public abstract unsafe class ResourceParam<T> : SystemParam where T : struct, IResource
{
    private object? Managed;

    private protected ResourceParam()
    {
    }

    private protected abstract bool bWrite { get; }

    private protected override void Describe(SystemParamBlock* Into, FieldInfo Field)
    {
        Into->Kind = SystemParamBlock.KindBound;
        if (ResourceType<T>.bNative)
        {
            ResourceType<T>.RequireNative();
        }
        else
        {
            ResourceNatives.GetNamedAccessToken(ResourceType<T>.Name);
        }
    }

    private protected override void OnBound()
    {
        ulong World = WorldHandle;
        if (World == 0)
        {
            return;
        }
        if (ResourceType<T>.bNative)
        {
            ResourceNatives.GetOrEmplaceResource(World, ResourceType<T>.RequireNative());
        }
        else
        {
            Managed = WorldResources.GetOrAdd(World, typeof(T));
        }
    }

    private protected override void Declare(CEntitySystem System)
    {
        if (bWrite)
        {
            System.DeclareWrite(ResourceType<T>.Name);
        }
        else
        {
            System.DeclareRead(ResourceType<T>.Name);
        }
    }

    internal ref T ManagedValue
    {
        get
        {
            if (ResourceType<T>.bNative || Managed == null)
            {
                throw Unbound();
            }
            return ref Unsafe.Unbox<T>(Managed);
        }
    }

    internal nint Address
    {
        get
        {
            ulong World = WorldHandle;
            nint Found = World != 0 && ResourceType<T>.bNative ? ResourceNatives.FindResource(World, ResourceType<T>.RequireNative()) : 0;
            return Found != 0 ? Found : throw Unbound();
        }
    }

    private static InvalidOperationException Unbound()
        => new($"{typeof(T).Name} is reachable only once its system is configured in a world that still holds it.");
}

// A system field reading the world's T, so the system runs beside every other reader of it, like the C++ TRes.
public sealed class TRes<T> : ResourceParam<T> where T : struct, IResource
{
    private protected override bool bWrite => false;
}

// A system field writing the world's T, so no other system touching T runs beside it, like the C++ TResMut.
public sealed class TResMut<T> : ResourceParam<T> where T : struct, IResource
{
    private protected override bool bWrite => true;
}

// A C# struct resource's Value; a native view's generated extension is not generic, so it wins for that type.
public static class ResourceAccess
{
    extension<T>(TRes<T> Param) where T : struct, IResource
    {
        public ref readonly T Value => ref Param.ManagedValue;
    }

    extension<T>(TResMut<T> Param) where T : struct, IResource
    {
        public ref T Value => ref Param.ManagedValue;
    }
}
