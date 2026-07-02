using System;
using LuminaSharp;

namespace Lumina;

/// <summary>
/// Handwritten extensions to the REFLECTED <see cref="CWorld"/> wrapper.
/// </summary>
public unsafe partial class CWorld
{
    private ulong WorldHandle => (ulong)Handle.ToInt64();
    
    public EntityRegistry Registry => new(WorldHandle);
    public Physics Physics => new(WorldHandle);
    public Camera Camera => new(WorldHandle);
    public DebugDraw Draw => new(WorldHandle);
    public Net Net => new(WorldHandle);
    public UI UI => new(WorldHandle);
    public Navigation Navigation => new(WorldHandle);
    public Perception Perception => new(WorldHandle);
    public GameplayMessageBus Messages => new(WorldHandle);
    public GameplayTags Tags => new(WorldHandle);
    public Audio Audio => new(WorldHandle);
    public Timers Timers => new(WorldHandle);
    public LuminaSharp.Animation Animation => new(WorldHandle);

    public float DeltaTime => (float)GetWorldDeltaTime();
    public double ElapsedTime => GetTimeSinceWorldCreation();

    /// <summary>Pauses gameplay (systems, scripts, physics). UI keeps running, so a pause menu built with
    /// <see cref="UI"/> can still set this back to false.</summary>
    public bool Paused
    {
        get => IsPaused();
        set => SetPaused(value);
    }

    /// <summary>World time scale: below 1 is slow motion, above 1 speeds the world up. Scales delta time
    /// for systems, scripts, and physics. Clamped to 0 or greater natively.</summary>
    public float TimeDilation
    {
        get => GetTimeDilation();
        set => SetTimeDilation(value);
    }

    /// <summary>
    /// Spawns the prefab at <paramref name="Path"/> and places its root at <paramref name="Location"/>
    /// (optionally rotated, optionally parented) in one call -- composes the generated
    /// <see cref="SpawnPrefab(string)"/> with SetEntityLocation/SetEntityRotation/SetParent. Returns the
    /// spawned root entity, or <see cref="Entity.Null"/> if the prefab couldn't be spawned.
    /// </summary>
    public Entity SpawnPrefab(string Path, FVector3 Location, FQuat? Rotation = null, Entity? Parent = null)
    {
        Entity Spawned = SpawnPrefab(Path);
        if (Spawned.IsNull)
        {
            return Spawned;
        }
        SetEntityLocation(Spawned, Location);
        if (Rotation.HasValue)
        {
            SetEntityRotation(Spawned, Rotation.Value);
        }
        if (Parent.HasValue)
        {
            SetParent(Spawned, Parent.Value);
        }
        return Spawned;
    }

    /// <summary>Spawn a projectile at <paramref name="position"/> moving at <paramref name="velocity"/>
    /// (world m/s). It sweeps forward each frame, reports its first hit, and auto-despawns after
    /// <paramref name="lifetime"/> seconds. Read or bind its hit via
    /// <c>Registry.Get&lt;ProjectileComponent&gt;(entity).OnHit</c>.</summary>
    public Entity SpawnProjectile(FVector3 position, FVector3 velocity, float damage = 0.0f, float lifetime = 5.0f)
        => SpawnProjectile(position, velocity, damage, lifetime, Entity.Null);

    // Mirrors the C++ CWorld wrappers so scripts reach components straight off the world (or an
    // EntityScript) instead of an entity-registry object. Each forwards to the per-world component store.

    /// <summary>The component of type T on the entity, or null if absent (mirrors C++ TryGetComponent).</summary>
    public T? TryGet<T>(Entity Entity) where T : NativeStruct => Registry.TryGet<T>(Entity);

    /// <summary>The component of type T on the entity; throws if absent (mirrors C++ GetComponent).</summary>
    public T Get<T>(Entity Entity) where T : NativeStruct => Registry.Get<T>(Entity);

    /// <summary>True if the entity has a T component.</summary>
    public bool Has<T>(Entity Entity) where T : NativeStruct => Registry.Has<T>(Entity);

    /// <summary>Get-or-emplace a default T and return the live wrapper (null for a tag).</summary>
    public T? Emplace<T>(Entity Entity) where T : NativeStruct => Registry.Emplace<T>(Entity);

    /// <summary>Alias of <see cref="Emplace{T}"/>.</summary>
    public T? Add<T>(Entity Entity) where T : NativeStruct => Registry.Add<T>(Entity);

    /// <summary>The T component, adding a default one first if absent.</summary>
    public T? GetOrAdd<T>(Entity Entity) where T : NativeStruct => Registry.GetOrAdd<T>(Entity);

    /// <summary>Remove the T component; returns true if one was present.</summary>
    public bool Remove<T>(Entity Entity) where T : NativeStruct => Registry.Remove<T>(Entity);

    /// <summary>Pulse on_update for the entity's T component (mutate via Get first, then Patch).</summary>
    public void Patch<T>(Entity Entity) where T : NativeStruct => Registry.Patch<T>(Entity);

    /// <summary>Fires when a T component is added to an entity.</summary>
    public RegistrySubscription OnConstruct<T>(Action<Entity> Callback) where T : NativeStruct => Registry.OnConstruct<T>(Callback);

    /// <summary>Fires when a T component is removed (or its entity destroyed).</summary>
    public RegistrySubscription OnDestroy<T>(Action<Entity> Callback) where T : NativeStruct => Registry.OnDestroy<T>(Callback);

    /// <summary>Fires when a T component is patched/replaced.</summary>
    public RegistrySubscription OnUpdate<T>(Action<Entity> Callback) where T : NativeStruct => Registry.OnUpdate<T>(Callback);

    // entt-style typed views (mirror registry.view<...>); pass World.Exclude<...>() to filter. Arity 1..4.
    public View<T1> View<T1>(Exclude Filter = default)
        where T1 : NativeStruct => Registry.View<T1>(Filter);

    public View<T1, T2> View<T1, T2>(Exclude Filter = default)
        where T1 : NativeStruct where T2 : NativeStruct => Registry.View<T1, T2>(Filter);

    public View<T1, T2, T3> View<T1, T2, T3>(Exclude Filter = default)
        where T1 : NativeStruct where T2 : NativeStruct where T3 : NativeStruct => Registry.View<T1, T2, T3>(Filter);

    public View<T1, T2, T3, T4> View<T1, T2, T3, T4>(Exclude Filter = default)
        where T1 : NativeStruct where T2 : NativeStruct where T3 : NativeStruct where T4 : NativeStruct => Registry.View<T1, T2, T3, T4>(Filter);

    /// <summary>Shorthand for <see cref="View{T1}(Exclude)"/>: iterate every entity that has a T component.</summary>
    public View<T1> All<T1>(Exclude Filter = default) where T1 : NativeStruct => Registry.All<T1>(Filter);

    // Exclude-filter builders for a View (mirror entt::exclude<...>). Arity 1..3.
    public static Exclude Exclude<T1>()
        where T1 : NativeStruct => EntityRegistry.Exclude<T1>();

    public static Exclude Exclude<T1, T2>()
        where T1 : NativeStruct where T2 : NativeStruct => EntityRegistry.Exclude<T1, T2>();

    public static Exclude Exclude<T1, T2, T3>()
        where T1 : NativeStruct where T2 : NativeStruct where T3 : NativeStruct => EntityRegistry.Exclude<T1, T2, T3>();
}
