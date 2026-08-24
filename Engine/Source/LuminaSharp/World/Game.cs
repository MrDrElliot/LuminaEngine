using System;

namespace LuminaSharp;

/// <summary>
/// Ambient access to the world the current gameplay callback belongs to. The runtime sets this around every
/// EntityScript and EntitySystem callback, so the static engine APIs (<see cref="Time"/>, <see cref="Sound"/>,
/// <see cref="Trace"/>, <see cref="Gizmo"/>) and the entity extension methods resolve their world without you
/// threading one through. Game-thread only, never touch it from a worker Task body.
/// </summary>
public static partial class Game
{
    /// <summary>
    /// Switches to another level. The world swap is deferred to the next frame start, so this is safe to
    /// call from any script callback; everything in the current world (entities, scripts, timers) is torn
    /// down and the new world starts fresh. URL forms: a world asset path ("/Game/Maps/Arena"), a hosted
    /// map ("/Game/Maps/Arena?listen?port=7777"), or a server address to connect to ("192.168.1.5:7777").
    /// </summary>
    public static void OpenLevel(string Url) => OpenLevelRaw(Url);

    /// <summary>
    /// Quits the game: exits the process in a packaged game; in the editor it ends the Play session
    /// instead. Deferred to a safe frame point, so it is fine to call from any script callback.
    /// </summary>
    public static void Quit() => QuitRaw();

    /// The persistent game instance, or null before the project has created one.
    public static Lumina.CGameInstance? Instance
    {
        get
        {
            IntPtr Handle = GetInstanceRaw();
            return Handle == IntPtr.Zero ? null : Wrapper<Lumina.CGameInstance>.ForObject(Handle);
        }
    }

    /// The game instance as your own subclass, or null when the project runs a different one.
    public static T? GetInstance<T>() where T : Lumina.CGameInstance => Instance as T;

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Game_GetInstance")]
    private static partial IntPtr GetInstanceRaw();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Game_OpenLevel")]
    private static partial void OpenLevelRaw(string Url);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Game_Quit")]
    private static partial void QuitRaw();

    [ThreadStatic] private static Lumina.CWorld? ActiveWorld;
    [ThreadStatic] private static Entity ActiveEntity;
    [ThreadStatic] private static bool ActiveHasEntity;
    [ThreadStatic] private static EntityScript? ActiveScriptField;

    /// <summary>The world the current callback runs in. Throws if accessed outside a gameplay callback.</summary>
    public static Lumina.CWorld World => ActiveWorld ?? throw new InvalidOperationException(
        "No active world: Game.World / Time / Sound / Trace / Gizmo are only valid inside a script or system callback.");

    /// <summary>True while a gameplay callback is running (so <see cref="World"/> is available).</summary>
    public static bool InWorld => ActiveWorld != null;

    // The entity whose script callback is running (Entity.Null in a system tick). Used by Trace.IgnoreSelf.
    internal static Entity CurrentEntity => ActiveHasEntity ? ActiveEntity : Entity.Null;

    // The script whose callback is currently running, or null.
    internal static EntityScript? ActiveScript => ActiveScriptField;

    internal static Scope Push(Lumina.CWorld World, Entity Entity)
    {
        Scope Prior = new(ActiveWorld, ActiveEntity, ActiveHasEntity, ActiveScriptField);
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = null;
        return Prior;
    }

    internal static Scope Push(Lumina.CWorld World, Entity Entity, EntityScript Script)
    {
        Scope Prior = new(ActiveWorld, ActiveEntity, ActiveHasEntity, ActiveScriptField);
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = Script;
        return Prior;
    }

    internal static Scope PushWorld(Lumina.CWorld World)
    {
        Scope Prior = new(ActiveWorld, ActiveEntity, ActiveHasEntity, ActiveScriptField);
        ActiveWorld = World;
        ActiveHasEntity = false;
        ActiveScriptField = null;
        return Prior;
    }

    internal readonly struct Scope : IDisposable
    {
        private readonly Lumina.CWorld? World;
        private readonly Entity Entity;
        private readonly bool HasEntity;
        private readonly EntityScript? Script;

        internal Scope(Lumina.CWorld? World, Entity Entity, bool HasEntity, EntityScript? Script)
        {
            this.World = World;
            this.Entity = Entity;
            this.HasEntity = HasEntity;
            this.Script = Script;
        }

        public void Dispose()
        {
            ActiveWorld = World;
            ActiveEntity = Entity;
            ActiveHasEntity = HasEntity;
            ActiveScriptField = Script;
        }
    }
}
