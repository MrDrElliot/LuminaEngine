using System;
using System.Collections.Generic;
using LuminaSharp;

namespace Lumina;

/// <summary>
/// Handwritten extensions to the REFLECTED <see cref="CWorld"/> wrapper.
/// </summary>
public unsafe partial class CWorld
{
    internal ulong WorldHandle => (ulong)Handle.ToInt64();
    
    public EntityRegistry Registry => new(WorldHandle);
    public UI UI => new(WorldHandle);
    public GameplayMessageBus Messages => new(this);

    public Tweens Tweens => new(this);

    // The subsystem of type T in this world, or null when none was created. Finds a C++ one just the same.
    public T? GetSubsystem<T>() where T : NativeObject
        => CGameLibrary.GetSubsystem(this, typeof(T).Name) as T;

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

    /// Spawns a prefab at the origin, unparented. Null entity if the prefab could not be spawned.
    public Entity SpawnPrefab(string Path) => CPrefabLibrary.SpawnPrefab(this, Path);

    /// Spawns a prefab already placed, in one native call. Null entity if the prefab could not be spawned.
    public Entity SpawnPrefab(string Path, FVector3 Location, FQuat? Rotation = null, Entity? Parent = null)
        => SpawnPrefab(Path, new FTransform(Location, Rotation ?? FQuat.Identity, FVector3.One), Parent);

    /// Spawns a prefab at a full transform, so an authored scale survives the spawn.
    public Entity SpawnPrefab(string Path, FTransform Transform, Entity? Parent = null)
        => CPrefabLibrary.SpawnPrefabAt(this, Path, Transform, Parent ?? Entity.Null);

    /// Spawns a prefab and runs Configure on the root before the next frame, so the values are in place for OnReady.
    public Entity SpawnPrefab(string Path, FTransform Transform, Action<Entity> Configure, Entity? Parent = null)
    {
        Entity Spawned = CPrefabLibrary.SpawnPrefabAt(this, Path, Transform, Parent ?? Entity.Null);
        if (!Spawned.IsNull)
        {
            Configure(Spawned);
        }
        return Spawned;
    }

    // Prefabs are authored as an asset reference, so the reference types spawn without unwrapping to a path.

    public Entity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab) => SpawnPrefab(Prefab.Path.Path);

    public Entity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab, FVector3 Location, FQuat? Rotation = null, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path.Path, Location, Rotation, Parent);

    public Entity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab, FTransform Transform, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path.Path, Transform, Parent);

    public Entity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab, FTransform Transform, Action<Entity> Configure, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path.Path, Transform, Configure, Parent);

    public Entity SpawnPrefab(FSoftObjectPath Prefab) => SpawnPrefab(Prefab.Path);

    public Entity SpawnPrefab(FSoftObjectPath Prefab, FVector3 Location, FQuat? Rotation = null, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path, Location, Rotation, Parent);

    public Entity SpawnPrefab(FSoftObjectPath Prefab, FTransform Transform, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path, Transform, Parent);

    public Entity SpawnPrefab(FSoftObjectPath Prefab, FTransform Transform, Action<Entity> Configure, Entity? Parent = null)
        => SpawnPrefab(Prefab.Path, Transform, Configure, Parent);

    /// Destroys the entity after Seconds. Zero or less leaves it alone; a second call retimes the countdown.
    public void SetLifetime(Entity Entity, float Seconds) => SetEntityLifetime(Entity, Seconds);

    /// The first entity carrying Tag, or the null entity. Order is unspecified when several share the tag.
    public Entity FindByTag(string Tag) => GetEntityByTag(Tag);

    /// Every entity carrying Tag. A fresh list per call; empty when nothing carries it.
    public List<Entity> FindAllByTag(string Tag) => new(GetEntitiesByTag(Tag));

    // Every script of type T attached anywhere in this world, an abstract base matching each subclass. A fresh list per call.
    public List<T> Scripts<T>() where T : CEntityScript
    {
        TSubclassOf<CEntityScript> Class = LuminaSharp.NativeClass<T>.Of.AsBase<CEntityScript>();
        CEntityScript[] Found = CEntityScriptLibrary.FindScriptsInWorld(this, Class.IsValid ? Class : LuminaSharp.NativeClass<CEntityScript>.Of.AsBase<CEntityScript>());
        var Result = new List<T>(Found.Length);
        foreach (CEntityScript Script in Found)
        {
            T? Typed = Script as T ?? (Class.IsValid ? LuminaSharp.Wrapper<T>.ForObject(Script.Handle) : null);
            if (Typed != null)
            {
                Result.Add(Typed);
            }
        }
        return Result;
    }

    // A new entity under Parent at LocalOffset in the parent's space. It goes when the parent is destroyed, so it needs no cleanup of its own.
    public Entity CreateChild(Entity Parent, string Name, FVector3 LocalOffset = default)
    {
        Entity Child = CreateEntity(Name);
        SetParent(Child, Parent);
        Registry.Get<STransformComponent>(Child).SetLocalLocation(LocalOffset);
        return Child;
    }

    // The first script of type T in this world, for the one-of-a-kind ones like a game director, or null.
    public T? FindScript<T>() where T : CEntityScript
    {
        List<T> Found = Scripts<T>();
        return Found.Count > 0 ? Found[0] : null;
    }

    /// A new named entity with a transform and nothing else; add components through <see cref="Registry"/>, destroy it with <see cref="DestroyEntity(Entity)"/>.
    public Entity CreateEntity(string Name, FVector3 Location = default, FQuat? Rotation = null, FVector3? Scale = null)
        => ConstructEntity(Name, new FTransform(Location, Rotation ?? FQuat.Identity, Scale ?? FVector3.One));

    /// A new named entity at the given transform.
    public Entity CreateEntity(string Name, FTransform Transform)
        => ConstructEntity(Name, Transform);

    /// A projectile that sweeps forward each frame, reports its first hit through its component's OnHit, and despawns after lifetime seconds.
    public Entity SpawnProjectile(FVector3 position, FVector3 velocity, float damage = 0.0f, float lifetime = 5.0f)
        => CProjectileLibrary.SpawnProjectile(this, position, velocity, damage, lifetime, Entity.Null);

}
