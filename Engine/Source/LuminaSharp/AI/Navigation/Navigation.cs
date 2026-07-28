using System;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// A world's navigation interface (<c>World.Navigation</c>). Wraps the engine's Recast/Detour navmesh:
/// path queries, point projection, reachability, plus convenience helpers for driving an entity along a
/// path and rebuilding navigation from a script. Every query dispatches to the first ready
/// <c>SNavMeshComponent</c> in the world; with no baked navmesh present each query simply reports "not
/// found" (never throws). Game thread only. Each member forwards to a flat <c>LuminaSharp_Nav_*</c> shim
/// in the Runtime module (DotNetGameplay.cpp), with the world Handle passed first.
/// </summary>
public readonly unsafe partial struct Navigation
{
    internal readonly ulong Handle;

    internal Navigation(ulong Handle)
    {
        this.Handle = Handle;
    }

    /// <summary>Upper bound on the corner count a single path query can return.</summary>
    public const int MaxPathCorners = 64;

    public bool IsValid => Handle != 0;

    /// <summary>True once a navmesh has been baked and finished hydrating, so queries can succeed.</summary>
    public bool IsReady => IsReadyNative() != 0;

    /// <summary>
    /// Finds a path from <paramref name="Start"/> to <paramref name="End"/>, writing the corners into the
    /// caller-supplied <paramref name="Corners"/> buffer. Returns the number of corners written (0 if no
    /// path was found); <paramref name="Partial"/> is set when the path stops short of the goal.
    /// Allocation-free: pass a <c>stackalloc</c> span.
    /// </summary>
    public int FindPath(FVector3 Start, FVector3 End, Span<FVector3> Corners, out bool Partial)
    {
        NavPathWire Wire = FindPathRaw(Start, End, Corners);
        Partial = Wire.Partial != 0;
        return Wire.Valid != 0 ? Wire.Count : 0;
    }

    /// <summary>
    /// Allocating convenience: finds a path and returns it as a <see cref="NavPath"/>, or null if none
    /// exists. For per-frame queries prefer the <see cref="Span{T}"/> overload to avoid the allocation.
    /// </summary>
    public NavPath? FindPath(FVector3 Start, FVector3 End)
    {
        Span<FVector3> Buffer = stackalloc FVector3[MaxPathCorners];
        NavPathWire Wire = FindPathRaw(Start, End, Buffer);
        if (Wire.Valid == 0 || Wire.Count <= 0)
        {
            return null;
        }

        FVector3[] Corners = new FVector3[Wire.Count];
        for (int i = 0; i < Wire.Count; ++i)
        {
            Corners[i] = Buffer[i];
        }
        return new NavPath(Corners, Wire.Partial != 0);
    }

    /// <summary>
    /// Snaps <paramref name="Point"/> onto the nearest walkable navmesh surface within the search box
    /// <paramref name="Extents"/> (half-extents), or null if nothing walkable is nearby.
    /// </summary>
    public FVector3? ProjectPoint(FVector3 Point, FVector3 Extents)
    {
        NavPointWire Wire = ProjectPointRaw(Point, Extents);
        return Wire.Found != 0 ? Wire.Point : null;
    }

    /// <summary>Projects with a default search box (generous on the vertical axis).</summary>
    public FVector3? ProjectPoint(FVector3 Point) => ProjectPoint(Point, new FVector3(2.0f, 16.0f, 2.0f));

    /// <summary>
    /// Walks the navmesh surface from <paramref name="Start"/> toward <paramref name="End"/> and returns
    /// the point where the walkable surface ends (a wall/edge), or null if <paramref name="End"/> is
    /// directly reachable. Useful for "how far can I move in this direction" checks.
    /// </summary>
    public FVector3? Raycast(FVector3 Start, FVector3 End)
    {
        NavPointWire Wire = RaycastRaw(Start, End);
        return Wire.Found != 0 ? Wire.Point : null;
    }

    /// <summary>A random walkable point within <paramref name="Radius"/> of <paramref name="Origin"/>, or null.</summary>
    public FVector3? FindRandomReachablePoint(FVector3 Origin, float Radius)
    {
        NavPointWire Wire = FindRandomRaw(Origin, Radius);
        return Wire.Found != 0 ? Wire.Point : null;
    }

    /// <summary>True if a complete (non-partial) path exists between the two points.</summary>
    public bool IsReachable(FVector3 From, FVector3 To) => IsReachableNative(From, To) != 0;

    /// <summary>Total length of the path between two points in world units, or a negative value if unreachable.</summary>
    public float PathLength(FVector3 From, FVector3 To) => PathLengthNative(From, To);

    /// <summary>
    /// Flags every navmesh volume in the world for an async rebuild (picked up next tick). Call after
    /// spawning or moving geometry that should affect navigation. Returns the number of volumes flagged.
    /// </summary>
    public int Rebuild() => RequestRebuildNative();

    /// <summary>Draws a debug polyline along the path between two points (for tuning/visualization).</summary>
    public void DrawPath(FVector3 From, FVector3 To, FVector4 Color, float Duration = 0.0f)
        => DrawPathNative(From, To, Color, Duration);

    // --- Agent helpers ------------------------------------------------------------------------------
    // Ensure-and-drive shortcuts over SPathFollowComponent. The agent also needs a character-controller
    // component for the movement input the path-follow system writes to actually move it.

    /// <summary>
    /// Sends <paramref name="Agent"/> to a static world location: ensures it has a path-follow component
    /// (idempotent) and sets the goal. The path-follow system repaths and steers it each tick. The agent
    /// needs a <c>SCharacterControllerComponent</c> for the movement to take effect.
    /// </summary>
    public SPathFollowComponent MoveTo(Entity Agent, FVector3 Destination, float Speed = 1.0f)
    {
        SPathFollowComponent Follow = new EntityRegistry(Handle).Emplace<SPathFollowComponent>(Agent)!;
        Follow.Speed = Speed;
        Follow.SetTargetLocation(Destination);
        return Follow;
    }

    /// <summary>
    /// Makes <paramref name="Agent"/> chase <paramref name="Target"/>: the path-follow system re-projects
    /// the target's location and repaths as it moves. See <see cref="MoveTo"/> for the controller note.
    /// </summary>
    public SPathFollowComponent Follow(Entity Agent, Entity Target, float Speed = 1.0f)
    {
        SPathFollowComponent Follow = new EntityRegistry(Handle).Emplace<SPathFollowComponent>(Agent)!;
        Follow.Speed = Speed;
        Follow.SetTargetEntity(Target);
        return Follow;
    }

    /// <summary>Clears <paramref name="Agent"/>'s goal and cached path, halting path following. No-op if it has none.</summary>
    public void StopMoving(Entity Agent)
    {
        SPathFollowComponent? Follow = new EntityRegistry(Handle).TryGet<SPathFollowComponent>(Agent);
        Follow?.Stop();
    }

    // Flat shims (Runtime module). The world Handle is the first native argument; FVector3/FVector4 pass
    // by value; the FindPath corner span expands to (FVector3*, int). See DotNetGameplay.cpp.

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_IsReady")]
    private partial int IsReadyNative();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_FindPath")]
    private partial NavPathWire FindPathRaw(FVector3 Start, FVector3 End, Span<FVector3> Corners);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_ProjectPoint")]
    private partial NavPointWire ProjectPointRaw(FVector3 Point, FVector3 Extents);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_Raycast")]
    private partial NavPointWire RaycastRaw(FVector3 Start, FVector3 End);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_FindRandomReachablePoint")]
    private partial NavPointWire FindRandomRaw(FVector3 Origin, float Radius);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_IsReachable")]
    private partial int IsReachableNative(FVector3 From, FVector3 To);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_PathLength")]
    private partial float PathLengthNative(FVector3 From, FVector3 To);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_RequestRebuild")]
    private partial int RequestRebuildNative();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Nav_DrawPath")]
    private partial void DrawPathNative(FVector3 From, FVector3 To, FVector4 Color, float Duration);
}

/// <summary>A navmesh path: an ordered list of world-space corners from start to goal.</summary>
public sealed class NavPath
{
    /// <summary>Corner points, in order. The first is the start, the last is the goal (or nearest reachable point).</summary>
    public readonly FVector3[] Corners;

    /// <summary>True when the path stops short of the requested goal (e.g. it was unreachable).</summary>
    public readonly bool IsPartial;

    internal NavPath(FVector3[] Corners, bool IsPartial)
    {
        this.Corners = Corners;
        this.IsPartial = IsPartial;
    }

    public int Count => Corners.Length;

    /// <summary>The final corner (the goal, or the nearest reachable point for a partial path).</summary>
    public FVector3 Destination => Corners[Corners.Length - 1];

    /// <summary>Summed length of the path in world units.</summary>
    public float Length
    {
        get
        {
            float Total = 0.0f;
            for (int i = 1; i < Corners.Length; ++i)
            {
                Total += FVector3.Distance(Corners[i], Corners[i - 1]);
            }
            return Total;
        }
    }
}

/// <summary>Blittable mirror of the native FLmNavPath (DotNetGameplay.cpp); the FindPath thunk's ABI return.</summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("NavPathWire")]
internal struct NavPathWire
{
    public int Count;
    public int Valid;
    public int Partial;
}

/// <summary>Blittable mirror of the native FLmNavPoint; the project/raycast/random thunk return.</summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("NavPointWire")]
internal struct NavPointWire
{
    public int Found;
    public FVector3 Point;
}
