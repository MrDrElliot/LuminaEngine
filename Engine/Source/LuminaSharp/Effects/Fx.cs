using Lumina;

namespace LuminaSharp;

/// Play particle effects in the current world, the visual counterpart to <see cref="Sound"/>.
public static class Fx
{
    /// Default seconds a one-shot effect entity lives before it is destroyed.
    public const float DefaultLifetime = 5.0f;

    /// Bursts System at a world point and despawns it after Lifetime seconds. Returns the effect entity.
    public static Entity Play(CParticleSystem? System, FVector3 Location, float Lifetime = DefaultLifetime)
        => Play(System, new FTransform(Location, FQuat.Identity, FVector3.One), Lifetime);

    /// Bursts System oriented along Normal, the shape an impact decal or spark wants.
    public static Entity PlayAligned(CParticleSystem? System, FVector3 Location, FVector3 Normal, float Lifetime = DefaultLifetime)
        => Play(System, new FTransform(Location, FQuat.FromToRotation(FVector3.Up, Normal), FVector3.One), Lifetime);

    /// Bursts System at a full transform, so a scaled or pre-rotated effect keeps its authored orientation.
    public static Entity Play(CParticleSystem? System, FTransform Transform, float Lifetime = DefaultLifetime)
        => System == null ? Entity.Null : Game.World.SpawnParticleSystem(System, Transform, Lifetime);

    /// Parents the effect to Target so it follows, optionally on a named socket or bone.
    public static Entity PlayAttached(CParticleSystem? System, Entity Target, string Socket = "",
        FVector3 Offset = default, float Lifetime = DefaultLifetime)
        => System == null ? Entity.Null : Game.World.SpawnParticleSystemAttached(System, Target, Socket, Offset, Lifetime);

    /// Resolves the reference (asset-manager cached) and plays it; a null or unset reference is a no-op.
    public static Entity Play(TSoftObjectPtr<CParticleSystem> System, FVector3 Location, float Lifetime = DefaultLifetime)
        => Play(System.Get(), Location, Lifetime);

    public static Entity PlayAligned(TSoftObjectPtr<CParticleSystem> System, FVector3 Location, FVector3 Normal, float Lifetime = DefaultLifetime)
        => PlayAligned(System.Get(), Location, Normal, Lifetime);

    public static Entity PlayAttached(TSoftObjectPtr<CParticleSystem> System, Entity Target, string Socket = "",
        FVector3 Offset = default, float Lifetime = DefaultLifetime)
        => PlayAttached(System.Get(), Target, Socket, Offset, Lifetime);

    /// Stops an effect entity emitting and lets its live particles finish, rather than cutting them off.
    public static void Stop(Entity Effect)
        => Game.World.Registry.TryGet<SParticleSystemComponent>(Effect)?.Deactivate();
}
