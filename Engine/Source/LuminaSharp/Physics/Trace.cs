using System;
using Lumina;

namespace LuminaSharp;

/// Fluent physics query, s&amp;box style, as Trace.Ray(a, b).Ignore(self).Run(). A mutable builder over CPhysicsLibrary that allocates nothing until Run or RunAll, against the ambient Engine.World.
public struct Trace
{
    private CWorld World;
    private FVector3 From;
    private FVector3 To;
    private float Radius;
    private Entity IgnoreEntity;
    private ECollisionProfiles Mask;

    private static Trace Begin(FVector3 From, FVector3 To)
    {
        Trace T = default;
        T.World = Engine.World;
        T.From = From;
        T.To = To;
        T.IgnoreEntity = Entity.Null;
        T.Mask = ECollisionProfiles.All;
        return T;
    }

    /// A ray between two world points.
    public static Trace Ray(FVector3 From, FVector3 To) => Begin(From, To);

    /// A ray from an origin along a direction for a distance.
    public static Trace Ray(FVector3 Origin, FVector3 Direction, float Distance)
        => Begin(Origin, Origin + Direction.Normalized() * Distance);

    /// A swept sphere, so a thick ray, between two points.
    public static Trace Sphere(float Radius, FVector3 From, FVector3 To)
    {
        Trace T = Begin(From, To);
        T.Radius = Radius;
        return T;
    }

    /// Skip one entity's body, typically the caster's.
    public Trace Ignore(Entity Entity)
    {
        IgnoreEntity = Entity;
        return this;
    }

    /// Skip the entity whose callback is running.
    public Trace IgnoreSelf()
    {
        IgnoreEntity = Engine.CurrentEntity;
        return this;
    }

    /// Only hit bodies whose collision layer intersects Mask. Ray traces only.
    public Trace WithMask(ECollisionProfiles Mask)
    {
        this.Mask = Mask;
        return this;
    }

    /// Runs the query and returns the closest hit. Check bHit on the result.
    public SRayResult Run()
    {
        if (Radius > 0.0f)
        {
            SRayResult[] Hits = CPhysicsLibrary.SphereCast(World, From, To, Radius, IgnoreEntity);
            return Hits.Length > 0 ? Hits[0] : default;
        }
        return CPhysicsLibrary.Raycast(World, From, To, IgnoreEntity, Mask);
    }

    /// Runs the query and returns every hit, near to far.
    public SRayResult[] RunAll()
    {
        if (Radius > 0.0f)
        {
            return CPhysicsLibrary.SphereCast(World, From, To, Radius, IgnoreEntity);
        }
        return CPhysicsLibrary.RaycastAll(World, From, To, IgnoreEntity, Mask);
    }
}
