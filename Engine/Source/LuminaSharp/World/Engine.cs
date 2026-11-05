using System;

namespace LuminaSharp;

/// <summary>
/// Ambient access to the world the current gameplay callback belongs to. The runtime sets this around every
/// CEntityScript, CWorldSubsystem and CExclusiveSystem callback, so the static engine APIs (<see cref="Time"/>, <see cref="Sound"/>,
/// <see cref="Trace"/>, <see cref="Gizmo"/>) and the Entity extension methods resolve their world without you
/// threading one through. A parallel CEntitySystem runs beside other systems, so it has no world here.
/// Game-thread only, never touch it from a worker Task body.
/// </summary>
public static partial class Engine
{
    // Switches level at the next frame start, keeping only SPersistentComponent entities, which arrive with their children and scripts and get OnTraveled.
    public static void OpenLevel(string Url) => World.OpenLevel(Url);

    /// <summary>
    /// Quits the game: exits the process in a packaged game; in the editor it ends the Play session
    /// instead. Deferred to a safe frame point, so it is fine to call from any script callback.
    /// </summary>
    public static void Quit() => Lumina.CGameLibrary.QuitGame();

    // The port a hosted session listens on and a join dials when the address names none.
    public const ushort DefaultPort = 7777;

    // Opens Map as a listen server, so this peer plays and hosts at once.
    public static void Host(string Map, ushort Port = DefaultPort) => OpenLevel($"{Map}?listen?port={Port}");

    // Connects to a host. An empty address means this machine, and one without a port gets DefaultPort.
    public static void Join(string Address, ushort Port = DefaultPort)
    {
        string Target = Address.Trim();
        if (Target.Length == 0)
        {
            Target = "127.0.0.1";
        }
        if (!Target.Contains(':'))
        {
            Target += $":{Port}";
        }
        OpenLevel(Target);
    }

    // A dedicated server or load-test process, which never opens a window or an audio device.
    public static bool IsHeadless => Lumina.CGameLibrary.IsHeadless();

    // False wherever nothing is drawn or heard, so cosmetic work can skip a server, a bot and a headless process alike.
    public static bool HasPresentation => InWorld ? World.HasPresentation() : !IsHeadless;

    [ThreadStatic] private static Lumina.CWorld? ActiveWorld;
    [ThreadStatic] private static FEntity ActiveEntity;
    [ThreadStatic] private static bool ActiveHasEntity;
    [ThreadStatic] private static Lumina.CEntityScript? ActiveScriptField;
    [ThreadStatic] private static object? EventTarget;
    [ThreadStatic] private static Lumina.CWorld? EventWorldCache;

    /// <summary>The world the current callback runs in. Throws if accessed outside a gameplay callback.</summary>
    public static Lumina.CWorld World => ActiveWorld ?? EventWorld() ?? throw new InvalidOperationException(
        "No active world: Engine.World / Time / Sound / Trace / Gizmo are only valid inside a script, world subsystem or exclusive system callback.");

    /// <summary>True while a gameplay callback is running (so <see cref="World"/> is available).</summary>
    public static bool InWorld => ActiveWorld != null || EventWorld() != null;

    // The entity whose script callback is running (FEntity.Null in a system tick). Used by Trace.IgnoreSelf.
    internal static FEntity CurrentEntity => ActiveHasEntity ? ActiveEntity : (EventTarget as Lumina.CEntityScript)?.Entity ?? FEntity.Null;

    // The script whose callback is currently running, or null.
    internal static Lumina.CEntityScript? ActiveScript => ActiveScriptField ?? EventTarget as Lumina.CEntityScript;

    // Resolved on first use, so an event that never touches the ambient world costs no native call.
    private static Lumina.CWorld? EventWorld()
    {
        return EventWorldCache ??= EventTarget switch
        {
            Lumina.CEntityScript Script => Script.World,
            Lumina.CWorldSubsystem Subsystem => Subsystem.World,
            Lumina.CExclusiveSystem System => System.GetWorld(),
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

    internal static Scope Push(Lumina.CWorld World, FEntity Entity)
    {
        Scope Prior = Capture();
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = null;
        EventTarget = null;
        return Prior;
    }

    internal static Scope Push(Lumina.CWorld World, FEntity Entity, Lumina.CEntityScript Script)
    {
        Scope Prior = Capture();
        ActiveWorld = World;
        ActiveEntity = Entity;
        ActiveHasEntity = true;
        ActiveScriptField = Script;
        EventTarget = null;
        return Prior;
    }

    // For a callback that belongs to a world but to no script, such as a UI listener registered outside one.
    internal static Scope Push(Lumina.CWorld World)
    {
        Scope Prior = Capture();
        ActiveWorld = World;
        ActiveHasEntity = false;
        ActiveScriptField = null;
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
        private readonly FEntity Entity;
        private readonly bool HasEntity;
        private readonly Lumina.CEntityScript? Script;
        private readonly object? Target;
        private readonly Lumina.CWorld? TargetWorld;

        internal Scope(Lumina.CWorld? World, FEntity Entity, bool HasEntity, Lumina.CEntityScript? Script, object? Target, Lumina.CWorld? TargetWorld)
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
