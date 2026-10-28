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

    // The world's event queues, for systems that drain them and scripts that send into them.
    public WorldEvents Events => new(this);

    // The subsystem of type T in this world, or null when none was created. A C++ one arrives as its base wrapper, so it is rewrapped as T.
    public T? GetSubsystem<T>() where T : NativeObject
        => GetSubsystem(NativeClass<T>.Of.AsBase<CWorldSubsystem>()) is { } Found ? Found as T ?? Wrapper<T>.ForObject(Found.Handle) : null;

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

    public List<T> Scripts<T>() where T : class
    {
        if (!typeof(T).IsInterface && !typeof(CEntityScript).IsAssignableFrom(typeof(T)))
        {
            return new List<T>();
        }

        TSubclassOf<CEntityScript> Class = typeof(CEntityScript).IsAssignableFrom(typeof(T))
            ? TSubclassOf<CEntityScript>.FromName(LuminaSharp.NativeTypeName.Of(typeof(T)))
            : default;
        CEntityScript[] Found = FindScriptsInWorld(Class.IsValid ? Class : LuminaSharp.NativeClass<CEntityScript>.Of.AsBase<CEntityScript>());
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

}
