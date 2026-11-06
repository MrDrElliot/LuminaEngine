using LuminaSharp;

namespace Lumina;

// The C# side of a script attached to one entity, the same CEntityScript a C++ script derives. Created by attaching it to an entity, never with new.
[DispatchWhen(nameof(OnAction), nameof(FeedsInputBindings))]
public partial class CEntityScript
{
    // Set when the managed instance is created, so dispatch reads cached labels and callback flags instead of reflecting.
    internal TypeDescription Description = null!;

    internal override void OnScriptInstanceCreated(ScriptableRuntime Runtime, System.Runtime.InteropServices.GCHandle Handle)
        => Runtime.EntityScripts.Adopt(this, Handle);

    // The native object owns both values and sets them once at attach, so the first real read is kept.
    private FEntity CachedEntity = FEntity.Null;
    private CWorld? CachedWorld;
    private TComponentRef<STransformComponent> CachedTransform;

    public FEntity Entity
    {
        get
        {
            if (CachedEntity.IsNull)
            {
                CachedEntity = GetOwningEntity();
            }
            return CachedEntity;
        }
    }

    public CWorld World => CachedWorld ??= GetWorld();

    // Lower runs first among this entity's C++ and C# scripts, and setting it in OnAttach orders the very first update.
    public int UpdatePriority
    {
        get => GetUpdatePriority();
        set => SetUpdatePriority(value);
    }

    private System.Threading.CancellationTokenSource? DestroyCts;

    // Canceled when this script is detached or destroyed, so a GameTask await given it stops cleanly.
    protected System.Threading.CancellationToken DestroyToken => (DestroyCts ??= new System.Threading.CancellationTokenSource()).Token;

    // The token GameTask falls back to, so an await started by a script ends with it even when no token was passed.
    internal System.Threading.CancellationToken LifetimeToken => DestroyToken;

    private System.Collections.Generic.List<System.IDisposable>? Owned;

    // Disposed after OnDetach in reverse order, so subscriptions made after a document go before it closes.
    protected T Own<T>(T Resource) where T : System.IDisposable
    {
        (Owned ??= new()).Add(Resource);
        return Resource;
    }

    protected void Own(System.Action OnRelease) => Own(new OwnedAction(OnRelease));

    private sealed class OwnedAction(System.Action OnRelease) : System.IDisposable
    {
        public void Dispose() => OnRelease();
    }

    internal void ReleaseLifetime()
    {
        if (DestroyCts != null)
        {
            DestroyCts.Cancel();
            DestroyCts.Dispose();
            DestroyCts = null;
        }

        if (Owned is { } Resources)
        {
            Owned = null;
            for (int Index = Resources.Count - 1; Index >= 0; --Index)
            {
                try
                {
                    Resources[Index].Dispose();
                }
                catch (System.Exception Exception)
                {
                    Native.Log(ELogLevel.Error, $"{GetType().Name} threw releasing an owned resource: {Exception}");
                }
            }
        }
    }

    // The reference follows its pool across relocation, so it is fetched again only once the component itself is gone.
    public ref STransformComponent Transform
    {
        get
        {
            if (!CachedTransform.IsValid)
            {
                CachedTransform = World.TryGetComponent<STransformComponent>(Entity).GetValueOrDefault();
            }
            return ref CachedTransform.Get();
        }
    }

    // The world subsystem of type T, which is where shared gameplay state belongs.
    protected T? GetSubsystem<T>() where T : NativeObject => World.GetSubsystem<T>();

    protected T? GetScript<T>(FEntity Target) where T : class
    {
        return World.GetScript<T>(Target);
    }

    protected T? GetScript<T>() where T : class
    {
        return World.GetScript<T>(Entity);
    }

    protected System.Collections.Generic.List<T> GetScripts<T>() where T : class
    {
        return World.GetScripts<T>(Entity);
    }

    protected T? AddScript<T>() where T : CEntityScript
    {
        return World.AddScript<T>(Entity);
    }

    protected bool RemoveScript<T>() where T : class
    {
        return World.RemoveScript<T>(Entity);
    }

    // The entity and world were set again by the level change, so the cached ones belong to a world that is gone.
    partial void __BeforeScriptEvent_OnTraveled()
    {
        CachedEntity = FEntity.Null;
        CachedWorld = null;
        CachedTransform = default;
    }

    // Feeds the script's SInputAction and SInputAxis bindings before its own OnAction runs.
    partial void __BeforeScriptEvent_OnAction(FName Action, SInputActionState State)
    {
        if (Description == null || !Description.HasInputBindings)
        {
            return;
        }

        try
        {
            using var Scope = LuminaSharp.Engine.Push(World, Entity, this);
            Description.DispatchAction(this, Action, in State);
        }
        catch (System.Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"CEntityScript input binding threw: {Exception}");
        }
    }

    private static bool FeedsInputBindings(TypeLibrary Library, System.Type ScriptType) => Library.Describe(ScriptType).HasInputBindings;

    // Add (idempotently) and return this entity's SInputComponent so it can receive input. Call in OnReady.
    protected ref SInputComponent EnableInput()
    {
        return ref World.GetOrEmplaceComponent<SInputComponent>(Entity);
    }

    // Remove this entity's SInputComponent, stopping OnInput and the input queries.
    protected void DisableInput()
    {
        World.RemoveComponent<SInputComponent>(Entity);
    }
}
