using Lumina;

namespace LuminaSharp;

/// Play particle effects in the current world, the visual counterpart to <see cref="Sound"/>.
public static class Fx
{
    /// Despawns a one-shot effect once its last particle dies. A streaming system then lives until the caller destroys it.
    public const float AutoLifetime = -1.0f;

    /// Bursts System at a world point and despawns it after Lifetime seconds. Returns the effect entity.
    public static Entity Play(CParticleSystem? System, FVector3 Location, float Lifetime = AutoLifetime)
        => Play(System, new FTransform(Location, FQuat.Identity, FVector3.One), Lifetime);

    /// Bursts System oriented along Normal, the shape an impact decal or spark wants.
    public static Entity PlayAligned(CParticleSystem? System, FVector3 Location, FVector3 Normal, float Lifetime = AutoLifetime)
        => Play(System, new FTransform(Location, FQuat.FromToRotation(FVector3.Up, Normal), FVector3.One), Lifetime);

    /// Bursts System at a full transform, so a scaled or pre-rotated effect keeps its authored orientation.
    public static Entity Play(CParticleSystem? System, FTransform Transform, float Lifetime = AutoLifetime)
        => System == null ? Entity.Null : CParticleSystemLibrary.SpawnParticleSystem(Game.World, System, Transform, Lifetime);

    /// Parents the effect to Target so it follows, optionally on a named socket or bone.
    public static Entity PlayAttached(CParticleSystem? System, Entity Target, string Socket = "",
        FVector3 Offset = default, float Lifetime = AutoLifetime)
        => System == null ? Entity.Null : CParticleSystemLibrary.SpawnParticleSystemAttached(Game.World, System, Target, Socket, Offset, Lifetime);

    /// Resolves the reference (asset-manager cached) and plays it; a null or unset reference is a no-op.
    public static Entity Play(TSoftObjectPtr<CParticleSystem> System, FVector3 Location, float Lifetime = AutoLifetime)
        => Play(System.Get(), Location, Lifetime);

    public static Entity PlayAligned(TSoftObjectPtr<CParticleSystem> System, FVector3 Location, FVector3 Normal, float Lifetime = AutoLifetime)
        => PlayAligned(System.Get(), Location, Normal, Lifetime);

    public static Entity PlayAttached(TSoftObjectPtr<CParticleSystem> System, Entity Target, string Socket = "",
        FVector3 Offset = default, float Lifetime = AutoLifetime)
        => PlayAttached(System.Get(), Target, Socket, Offset, Lifetime);

    /// Stops an effect entity emitting and lets its live particles finish, rather than cutting them off.
    public static void Stop(Entity Effect)
        => Game.World.Registry.TryGet<SParticleSystemComponent>(Effect)?.Deactivate();
}
