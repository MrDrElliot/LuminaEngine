using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Lumina;
using LuminaSharp.Rendering;

namespace LuminaSharp;

/// <summary>
/// Owns the live <see cref="RenderScene"/> instances for one loaded generation, mirroring
/// <see cref="EntitySystemRuntime"/>: the link to native is a strong <see cref="GCHandle"/> stored by the
/// FManagedRenderScene proxy, and every live handle is freed before the ALC unloads. Native tears the
/// proxies down BEFORE a reload (ManagedRenderScenes::PreScriptUnload), so FreeAll normally only runs work
/// at process shutdown.
/// </summary>
internal sealed class RenderSceneRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    public RenderSceneRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    /// <summary>Number of discovered RenderScene types in this generation.</summary>
    public int TypeCount => Library.RenderSceneTypes.Count;

    /// <summary>Reports every discovered RenderScene subclass to a native name sink, sorted by full name so
    /// the native "use the first one" pick is deterministic. Called once per (re)load.</summary>
    public unsafe void Enumerate(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Names = new List<string>(Library.RenderSceneTypes.Count);
        foreach (Type Type in Library.RenderSceneTypes)
        {
            if (Type.FullName is { } FullName)
            {
                Names.Add(FullName);
            }
        }
        Names.Sort(StringComparer.Ordinal);

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, void>)Sink;
        Span<byte> Scratch = stackalloc byte[256];
        foreach (string Name in Names)
        {
            Interop.FInteropString Encoded = new(Name, Scratch);
            try
            {
                Add(Context, Encoded.Pointer, Encoded.Length);
            }
            finally
            {
                Encoded.Free();
            }
        }
    }

    /// <summary>Instantiates the named RenderScene for a world and runs OnInit; returns a strong GCHandle
    /// (as IntPtr) the native proxy stores, or IntPtr.Zero on failure.</summary>
    public IntPtr Create(string TypeName, ulong World)
    {
        Type? Type = Library.GetRenderScene(TypeName);
        if (Type == null)
        {
            Native.Log(ELogLevel.Warn, $"RenderScene type not found: '{TypeName}'.");
            return IntPtr.Zero;
        }

        if (Activator.CreateInstance(Type) is not RenderScene Scene)
        {
            Native.Log(ELogLevel.Error, $"Failed to create RenderScene '{TypeName}'.");
            return IntPtr.Zero;
        }

        Scene.World = new Lumina.CWorld(new IntPtr(unchecked((long)World)));

        try
        {
            using (Game.PushWorld(Scene.World))
            {
                Scene.OnInit();
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.OnInit threw: {Exception}");
            return IntPtr.Zero;
        }

        GCHandle Handle = GCHandle.Alloc(Scene);
        LiveHandles.Add(Handle);
        return GCHandle.ToIntPtr(Handle);
    }

    /// <summary>Runs OnShutdown and frees the GCHandle. Called by the native proxy's Shutdown (rendering
    /// already flushed).</summary>
    public void Destroy(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return;
        }

        GCHandle Handle = GCHandle.FromIntPtr(Pointer);
        ShutdownScene(Handle);
        LiveHandles.Remove(Handle);
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
    }

    public unsafe void Extract(IntPtr Handle, IntPtr View)
    {
        if (Resolve(Handle) is not RenderScene Scene || View == IntPtr.Zero)
        {
            return;
        }

        try
        {
            using (Game.PushWorld(Scene.World))
            {
                Scene.OnExtract(in *(SceneView*)View);
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.OnExtract threw: {Exception}");
        }
    }

    public void Render(IntPtr Handle, int FrameIndex)
    {
        if (Resolve(Handle) is not RenderScene Scene)
        {
            return;
        }

        try
        {
            using (Game.PushWorld(Scene.World))
            {
                Scene.OnRender(FrameIndex);
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.OnRender threw: {Exception}");
        }
    }

    public void Resize(IntPtr Handle, uint Width, uint Height)
    {
        if (Resolve(Handle) is not RenderScene Scene)
        {
            return;
        }

        try
        {
            using (Game.PushWorld(Scene.World))
            {
                Scene.OnResize(Width, Height);
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.OnResize threw: {Exception}");
        }
    }

    public ulong GetDisplayTexture(IntPtr Handle)
    {
        try
        {
            return Resolve(Handle) is RenderScene Scene ? Scene.DisplayTexture.Handle : 0;
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.DisplayTexture threw: {Exception}");
            return 0;
        }
    }

    public uint GetDisplayResourceID(IntPtr Handle)
    {
        try
        {
            return Resolve(Handle) is RenderScene Scene ? Scene.DisplayResourceID : uint.MaxValue;
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.DisplayResourceID threw: {Exception}");
            return uint.MaxValue;
        }
    }

    public unsafe void GetExtent(IntPtr Handle, uint* Width, uint* Height)
    {
        if (Width == null || Height == null)
        {
            return;
        }

        *Width = 0;
        *Height = 0;
        try
        {
            if (Resolve(Handle) is RenderScene Scene)
            {
                FUIntVector2 Extent = Scene.RenderExtent;
                *Width = Extent.X;
                *Height = Extent.Y;
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.RenderExtent threw: {Exception}");
        }
    }

    /// <summary>Frees every live handle (running OnShutdown) so the collectible ALC can unload.</summary>
    public void FreeAll()
    {
        foreach (GCHandle Handle in LiveHandles)
        {
            ShutdownScene(Handle);
            if (Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
    }

    private static void ShutdownScene(GCHandle Handle)
    {
        if (Handle.Target is not RenderScene Scene)
        {
            return;
        }

        try
        {
            using (Game.PushWorld(Scene.World))
            {
                Scene.OnShutdown();
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"RenderScene.OnShutdown threw: {Exception}");
        }
    }

    private static RenderScene? Resolve(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return GCHandle.FromIntPtr(Pointer).Target as RenderScene;
    }
}
