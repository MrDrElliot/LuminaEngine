namespace LuminaSharp;

/// <summary>The physics phase an <see cref="EntityScript"/>'s OnUpdate runs in: PrePhysics (default) before the
/// physics step, PostPhysics after (read settled results, e.g. follow cameras).</summary>
public enum EScriptPhase
{
    PrePhysics,
    PostPhysics,
}

/// <summary>Declares which physics phase a script's OnUpdate runs in (default PrePhysics).</summary>
[System.AttributeUsage(System.AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
public sealed class UpdatePhaseAttribute : System.Attribute
{
    public EScriptPhase Phase { get; }

    public UpdatePhaseAttribute(EScriptPhase Phase)
    {
        this.Phase = Phase;
    }
}

/// <summary>
/// Base class for a script attached to a single entity.
///
/// This IS a native <c>CEntityScript</c>: a C# script is an ordinary CObject of a runtime-minted CClass
/// deriving the same base a C++ script derives, so the native driver ticks both through one loop of virtual
/// calls with no language-specific path. The lifecycle methods (OnAttach / OnReady / OnUpdate / OnFixedUpdate /
/// OnDetach) are inherited from that base as <c>[ScriptEvent]</c> virtuals -- override them as before.
///
/// Consequence worth knowing: an instance is only valid once the native side has created it. Scripts are
/// created by attaching them to an entity, never by <c>new</c>.
/// </summary>
public abstract class EntityScript : Lumina.CEntityScript
{
    internal TypeDescription Description = null!; // set at Create; cached labels + callback flags, no per-frame reflection

    /// <summary>This script's entity (mirrors C++ ECS::FEntity). Read from the native object, which is the
    /// single owner of the value for both languages.</summary>
    public Entity Entity => GetOwningEntity();

    /// <summary>The world this script lives in. Also read from the native object.</summary>
    public Lumina.CWorld World => GetWorld();

    /// <summary>The world's component store (mirrors C++ ECS::FRegistry).</summary>
    public EntityRegistry Registry => World.Registry;

    private System.Threading.CancellationTokenSource? DestroyCts;

    /// <summary>Cancelled when this script is detached/destroyed. Pass to <see cref="GameTask"/> calls so a
    /// pending <c>await</c> stops cleanly when the entity goes away.</summary>
    protected System.Threading.CancellationToken DestroyToken => (DestroyCts ??= new System.Threading.CancellationTokenSource()).Token;

    internal void CancelDestroyToken()
    {
        if (DestroyCts != null)
        {
            DestroyCts.Cancel();
            DestroyCts.Dispose();
            DestroyCts = null;
        }
    }

    // This entity's transform, resolved on each access because a view dies at the next structural change.
    public Lumina.STransformComponent Transform => Registry.Get<Lumina.STransformComponent>(Entity);


    // The world subsystem of type T, which is where shared gameplay state belongs.
    protected T? GetSubsystem<T>() where T : NativeObject => World.GetSubsystem<T>();

    /// <summary>Get the script of type T on another entity (or this one), or null.</summary>
    protected T? GetScript<T>(Entity Target) where T : EntityScript
    {
        return Registry.GetScript<T>(Target);
    }

    /// <summary>Get the script of type T on this entity, or null.</summary>
    protected T? GetScript<T>() where T : EntityScript
    {
        return Registry.GetScript<T>(Entity);
    }

    /// <summary>Every script of type T on this entity.</summary>
    protected System.Collections.Generic.List<T> GetScripts<T>() where T : EntityScript
    {
        return Registry.GetScripts<T>(Entity);
    }

    /// <summary>Attach a new script of type T to this entity and return it (null on failure).</summary>
    protected T? AddScript<T>() where T : EntityScript
    {
        return Registry.AddScript<T>(Entity);
    }

    /// <summary>Remove the first script of type T from this entity. Returns true if one was removed.</summary>
    protected bool RemoveScript<T>() where T : EntityScript
    {
        return Registry.RemoveScript<T>(Entity);
    }

    // OnAttach / OnReady / OnUpdate / OnFixedUpdate / OnDetach are inherited from Lumina.CEntityScript as
    // [ScriptEvent] virtuals, and overriding one sets its slot bit on the minted class so the native shim
    // only crosses the boundary for the callbacks a script actually implements.

    /// <summary>An authored action that changed this frame. The base feeds the script's SInputAction /
    /// SInputAxis bindings, so an override must call <c>base.OnAction</c> for those to keep firing.</summary>
    public override void OnAction(Lumina.FName Action, Lumina.FInputActionState State)
    {
        if (!Description.HasInputBindings)
        {
            return;
        }

        try
        {
            using var Scope = Game.Push(World, Entity, this);
            Description.DispatchAction(this, Action, in State);
        }
        catch (System.Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntityScript input binding threw: {Exception}");
        }
    }

    /// <summary>Add (idempotent) and return this entity's SInputComponent so it can receive input. Call in OnReady.</summary>
    protected Lumina.SInputComponent EnableInput()
    {
        return Registry.Emplace<Lumina.SInputComponent>(Entity) ?? Registry.Get<Lumina.SInputComponent>(Entity);
    }

    /// <summary>Remove this entity's SInputComponent, stopping OnInput and the input queries.</summary>
    protected void DisableInput()
    {
        Registry.Remove<Lumina.SInputComponent>(Entity);
    }

}
