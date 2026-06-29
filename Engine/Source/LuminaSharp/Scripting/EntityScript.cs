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

/// <summary>Base class for a script attached to a single entity.</summary>
public abstract class EntityScript
{
    internal TypeDescription Description = null!; // set at Create; cached labels + callback flags, no per-frame reflection

    /// <summary>This script's entity (mirrors C++ entt::entity).</summary>
    public Entity Entity { get; internal set; }

    /// <summary>The world this script lives in.</summary>
    public Lumina.CWorld World { get; internal set; } = null!;

    /// <summary>The world's component store (mirrors C++ FEntityRegistry / entt::registry).</summary>
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

    // Delegate bindings this script made; auto-detached when the script is destroyed.
    private System.Collections.Generic.List<DelegateBinding>? TrackedBindings;

    internal void TrackBinding(DelegateBinding Binding)
    {
        if (Binding.IsValid)
        {
            (TrackedBindings ??= new System.Collections.Generic.List<DelegateBinding>()).Add(Binding);
        }
    }

    internal void UnbindAllDelegates()
    {
        if (TrackedBindings == null)
        {
            return;
        }
        foreach (DelegateBinding Binding in TrackedBindings)
        {
            Binding.Unbind();
        }
        TrackedBindings.Clear();
    }

    private Lumina.STransformComponent? CachedTransform;

    /// <summary>This entity's transform, resolved once and cached (avoids a per-frame Get crossing + alloc).</summary>
    public Lumina.STransformComponent Transform => CachedTransform ??= Registry.Get<Lumina.STransformComponent>(Entity);

    // The owning world handle and this instance's own GCHandle (as IntPtr), set at Create. Used by the
    // multi-script index and RemoveScript.
    internal ulong WorldHandle;
    internal System.IntPtr SelfHandle;

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

    /// <summary>Called once when the script instance is attached to its entity.</summary>
    public virtual void OnAttach()
    {
    }

    /// <summary>Called once after OnAttach, before the first OnUpdate (all siblings are attached).</summary>
    public virtual void OnReady()
    {
    }

    /// <summary>Called every frame on the game thread while the owning entity is enabled.</summary>
    public virtual void OnUpdate(float DeltaTime)
    {
    }

    /// <summary>Called at the fixed physics timestep (0..N times/frame, before physics) for framerate-independent
    /// physics logic. <paramref name="FixedDeltaTime"/> is the fixed step (1 / physics Hz), not the frame delta.</summary>
    public virtual void OnFixedUpdate(float FixedDeltaTime)
    {
    }

    /// <summary>Called per discrete input event while the entity is receiving input (needs <see cref="EnableInput"/>).</summary>
    public virtual void OnInput(Lumina.InputEvent Event)
    {
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

    /// <summary>Called once when the script/entity is detached or destroyed.</summary>
    public virtual void OnDetach()
    {
    }
}
