using System;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// The per-tick context handed to an <see cref="EntitySystem"/>, wrapping the native
/// <c>const FSystemContext*</c> as an <see cref="IntPtr"/>. The native stage scheduler passes the
/// pointer through the shared system shim each frame; it is only valid for the duration of the OnUpdate
/// call. Every member is a <c>[NativeCall] partial</c> forwarding to a flat <c>LuminaSharp_SystemContext_*</c>
/// shim in the Runtime module (DotNetGameplay.cpp), with the context Handle passed first.
/// </summary>
public readonly unsafe partial struct SystemContext
{
    internal readonly IntPtr Handle;

    // Resolved once per context rather than on every Registry access.
    private readonly ulong World;

    internal SystemContext(IntPtr Handle)
    {
        this.Handle = Handle;
        World = 0; // GetWorld reads Handle through this, so every field must be assigned before the call
        World = Handle != IntPtr.Zero ? GetWorld() : 0;
    }

    public bool IsValid => Handle != IntPtr.Zero;

    // The world's component store, mirroring C++ ECS::FRegistry. Author typed views with Registry.View.
    public EntityRegistry Registry => new(World);

    /// <summary>Seconds since the previous frame.</summary>
    public float DeltaTime => GetDeltaTime();

    /// <summary>Seconds since the world was created.</summary>
    public double Time => GetTime();

    /// <summary>Creates a new entity and returns its handle.</summary>
    public Entity Create() => CreateEntity();

    /// <summary>Destroys an entity.</summary>
    public void Destroy(Entity Entity) => DestroyEntity(Entity);

    /// <summary>Sets an entity's world-space location.</summary>
    public void SetEntityLocation(Entity Entity, FVector3 Location) => SetLocation(Entity, Location);

    /// <summary>Draws a debug line for one frame (Dev/Debug only; a no-op in Shipping).</summary>
    public void DrawDebugLine(FVector3 Start, FVector3 End, FVector4 Color) => DrawLine(Start, End, Color);

    // Flat shims (Runtime module). The context Handle is the first native argument; entities are the ECS
    // ids (uint); FVector3/FVector4 pass by value. See DotNetGameplay.cpp / DotNetView.cpp.

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_GetWorld")]
    private partial ulong GetWorld();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_GetDeltaTime")]
    private partial float GetDeltaTime();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_GetTime")]
    private partial double GetTime();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_Create")]
    private partial Entity CreateEntity();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_Destroy")]
    private partial void DestroyEntity(Entity Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_SetEntityLocation")]
    private partial void SetLocation(Entity Entity, FVector3 Location);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_SystemContext_DrawDebugLine")]
    private partial void DrawLine(FVector3 Start, FVector3 End, FVector4 Color);
}
