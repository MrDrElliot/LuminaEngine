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
    public GameplayMessageBus MessageBus => new(this);

    // The world's event queues, for systems that drain them and scripts that send into them.
    public WorldEvents Events => new(this);

    // The subsystem of type T in this world, or null when none was created. Finds a C++ one just the same.
    public T? GetSubsystem<T>() where T : NativeObject
        => CGameLibrary.GetSubsystem(this, SubsystemName<T>.Value) as T;

    // Interned once per type, so a lookup sends an 8-byte id instead of text the native side re-interns.
    private static class SubsystemName<T>
    {
        public static readonly FName Value = typeof(T).Name;
    }

    public float DeltaTime => (float)GetWorldDeltaTime();
    public double ElapsedTime => GetTimeSinceWorldCreation();

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
    public FEntity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab) => CPrefabLibrary.SpawnPrefab(this, Prefab);

    /// Spawns a prefab already placed, in one native call. Null entity if the prefab could not be spawned.
    public FEntity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab, FVector3 Location, FQuat? Rotation = null, FEntity? Parent = null)
        => SpawnPrefab(Prefab, new FTransform(Location, Rotation ?? FQuat.Identity, FVector3.One), Parent);

    /// Spawns a prefab at a full transform, so an authored scale survives the spawn.
    public FEntity SpawnPrefab(TSoftObjectPtr<CPrefab> Prefab, FTransform Transform, FEntity? Parent = null)
        => CPrefabLibrary.SpawnPrefabAt(this, Prefab, Transform, Parent ?? FEntity.Null);

    /// Spawns a prefab already loaded, with no path lookup. Null entity if the prefab is null.
    public FEntity SpawnPrefab(CPrefab Prefab) => CPrefabLibrary.SpawnLoadedPrefab(this, Prefab);

    /// Spawns a prefab already loaded, placed in one native call.
    public FEntity SpawnPrefab(CPrefab Prefab, FVector3 Location, FQuat? Rotation = null, FEntity? Parent = null)
        => SpawnPrefab(Prefab, new FTransform(Location, Rotation ?? FQuat.Identity, FVector3.One), Parent);

    /// Spawns a prefab already loaded at a full transform, so an authored scale survives the spawn.
    public FEntity SpawnPrefab(CPrefab Prefab, FTransform Transform, FEntity? Parent = null)
        => CPrefabLibrary.SpawnLoadedPrefabAt(this, Prefab, Transform, Parent ?? FEntity.Null);

    public List<T> Scripts<T>() where T : class
    {
        if (!typeof(T).IsInterface && !typeof(CEntityScript).IsAssignableFrom(typeof(T)))
        {
            return new List<T>();
        }

        TSubclassOf<CEntityScript> Class = typeof(CEntityScript).IsAssignableFrom(typeof(T))
            ? TSubclassOf<CEntityScript>.FromName(LuminaSharp.NativeTypeName.Of(typeof(T)))
            : default;
        CEntityScript[] Found = CEntityScriptLibrary.FindScriptsInWorld(this, Class.IsValid ? Class : LuminaSharp.NativeClass<CEntityScript>.Of.AsBase<CEntityScript>());
        var Result = new List<T>(Found.Length);
        foreach (CEntityScript Script in Found)
        {
            T? Typed = Script as T ?? LuminaSharp.Wrapper<T>.ForObject(Script.Handle);
            if (Typed != null)
            {
                Result.Add(Typed);
            }
        }
        return Result;
    }

    // A new entity under Parent at LocalOffset in the parent's space. It goes when the parent is destroyed, so it needs no cleanup of its own.
    public FEntity CreateChild(FEntity Parent, string Name, FVector3 LocalOffset = default)
    {
        FEntity Child = CreateEntity(Name);
        SetParent(Child, Parent);
        GetComponent<STransformComponent>(Child).SetLocalLocation(LocalOffset);
        return Child;
    }

    // The first script of type T in this world, for the one-of-a-kind ones like a game director, or null.
    public T? FindScript<T>() where T : class
    {
        List<T> Found = Scripts<T>();
        return Found.Count > 0 ? Found[0] : null;
    }

    /// A new named entity with a transform and nothing else.
    public FEntity CreateEntity(string Name, FVector3 Location = default, FQuat? Rotation = null, FVector3? Scale = null)
        => ConstructEntity(Name, new FTransform(Location, Rotation ?? FQuat.Identity, Scale ?? FVector3.One));

    /// A projectile that sweeps forward each frame, reports its first hit through its component's OnHit, and despawns after lifetime seconds.
    public FEntity SpawnProjectile(FVector3 position, FVector3 velocity, float damage = 0.0f, float lifetime = 5.0f)
        => CProjectileLibrary.SpawnProjectile(this, position, velocity, damage, lifetime, FEntity.Null);

}
