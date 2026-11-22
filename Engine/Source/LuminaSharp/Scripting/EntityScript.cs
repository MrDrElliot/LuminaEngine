using LuminaSharp;

namespace Lumina;

// The C# side of a script attached to one entity, the same CEntityScript a C++ script derives. Created by attaching it to an entity, never with new.
[DispatchWhen(nameof(OnTraveled), nameof(AlwaysDispatched))]
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

    // The entity and world were set again by the level change, so the cached ones belong to a world that is gone, as does the component the bindings listened on.
    partial void __BeforeScriptEvent_OnTraveled()
    {
        CachedEntity = FEntity.Null;
        CachedWorld = null;
        CachedTransform = default;
        ListenForInputBindings();
    }

    private static bool AlwaysDispatched(TypeLibrary Library, System.Type ScriptType) => true;

    // A script method, so the binding ends by itself when this script detaches.
    internal void ListenForInputBindings()
    {
        if (Description != null && Description.HasInputBindings)
        {
            World.GetOrEmplaceComponent<SInputComponent>(Entity).OnAction += FeedInputBindings;
        }
    }

    private void FeedInputBindings(FName Action, SInputActionState State)
    {
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

    // Adds this entity's SInputComponent if missing and turns it on, so its OnInput and OnAction fire.
    protected ref SInputComponent EnableInput()
    {
        ref SInputComponent Input = ref World.GetOrEmplaceComponent<SInputComponent>(Entity);
        Input.bEnabled = true;
        return ref Input;
    }

    // Turns input off while keeping the component, so every listener on it, input bindings included, is still there when it comes back.
    protected void DisableInput()
    {
        if (World.TryGetComponent<SInputComponent>(Entity) is { } Input)
        {
            Input.Get().bEnabled = false;
        }
    }
}
