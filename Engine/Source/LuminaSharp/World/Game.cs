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
    public static void OpenLevel(string Url) => Lumina.CGameLibrary.OpenLevel(Url);

    /// <summary>
    /// Quits the game: exits the process in a packaged game; in the editor it ends the Play session
    /// instead. Deferred to a safe frame point, so it is fine to call from any script callback.
    /// </summary>
    public static void Quit() => Lumina.CGameLibrary.QuitGame();

    /// The persistent game instance, or null before the project has created one.
    public static Lumina.CGameInstance? Instance => Lumina.CGameLibrary.GetGameInstance();

    /// The game instance as your own subclass, or null when the project runs a different one.
    public static T? GetInstance<T>() where T : Lumina.CGameInstance => Instance as T;

    [ThreadStatic] private static Lumina.CWorld? ActiveWorld;
    [ThreadStatic] private static Entity ActiveEntity;
    [ThreadStatic] private static bool ActiveHasEntity;
    [ThreadStatic] private static EntityScript? ActiveScriptField;
    [ThreadStatic] private static object? EventTarget;
    [ThreadStatic] private static Lumina.CWorld? EventWorldCache;

    /// <summary>The world the current callback runs in. Throws if accessed outside a gameplay callback.</summary>
    public static Lumina.CWorld World => ActiveWorld ?? EventWorld() ?? throw new InvalidOperationException(
        "No active world: Game.World / Time / Sound / Trace / Gizmo are only valid inside a script or system callback.");

    /// <summary>True while a gameplay callback is running (so <see cref="World"/> is available).</summary>
    public static bool InWorld => ActiveWorld != null || EventWorld() != null;

    // The entity whose script callback is running (Entity.Null in a system tick). Used by Trace.IgnoreSelf.
    internal static Entity CurrentEntity => ActiveHasEntity ? ActiveEntity : (EventTarget as EntityScript)?.Entity ?? Entity.Null;

    // The script whose callback is currently running, or null.
    internal static EntityScript? ActiveScript => ActiveScriptField ?? EventTarget as EntityScript;

    // Resolved on first use, so an event that never touches the ambient world costs no native call.
    private static Lumina.CWorld? EventWorld()
    {
        return EventWorldCache ??= EventTarget switch
        {
            EntityScript Script => Script.World,
            WorldSubsystem Subsystem => Subsystem.World,
            _ => null,
        };
    }

    // Generated [ScriptEvent] thunks wrap every native-to-managed event call in this, so lifecycle callbacks see their world.
    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    public static Scope EnterScriptEvent(object Target)
    {
        Scope Prior = Capture();
        ActiveWorld = null;
        ActiveHasEntity = false;
        ActiveScriptField = null;
        EventTarget = Target;
        EventWorldCache = null;
        return Prior;
    }

    // EnterScriptEvent without the save, for a batch that captured once and only changes the target per script.
    internal static void RetargetScriptEvent(object Target)
    {
        ActiveWorld = null;
        ActiveHasEntity = false;
        ActiveScriptField = null;
        EventTarget = Target;
        EventWorldCache = null;
    }

    internal static Scope Push(Lumina.CWorld World, Entity Entity)
    {
        Scope Prior = Capture();
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = null;
        EventTarget = null;
        return Prior;
    }

    internal static Scope Push(Lumina.CWorld World, Entity Entity, EntityScript Script)
    {
        Scope Prior = Capture();
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = Script;
        EventTarget = null;
        return Prior;
    }

    private static Scope Capture() => new(ActiveWorld, ActiveEntity, ActiveHasEntity, ActiveScriptField, EventTarget, EventWorldCache);

    // The whole callback context, for code that resumes later and must see the same world, entity and script.
    internal static Scope Snapshot() => Capture();

    // Re-enters a snapshot, returning the context to restore once the resumed code is done.
    internal static Scope Resume(Scope Captured)
    {
        Scope Prior = Capture();
        Captured.Dispose();
        return Prior;
    }

    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    public readonly struct Scope : IDisposable
    {
        private readonly Lumina.CWorld? World;
        private readonly Entity Entity;
        private readonly bool HasEntity;
        private readonly EntityScript? Script;
        private readonly object? Target;
        private readonly Lumina.CWorld? TargetWorld;

        internal Scope(Lumina.CWorld? World, Entity Entity, bool HasEntity, EntityScript? Script, object? Target, Lumina.CWorld? TargetWorld)
        {
            this.World = World;
            this.Entity = Entity;
            this.HasEntity = HasEntity;
            this.Script = Script;
            this.Target = Target;
            this.TargetWorld = TargetWorld;
        }

        public void Dispose()
        {
            ActiveWorld = World;
            ActiveEntity = Entity;
            ActiveHasEntity = HasEntity;
            ActiveScriptField = Script;
            EventTarget = Target;
            EventWorldCache = TargetWorld;
        }
    }
}
