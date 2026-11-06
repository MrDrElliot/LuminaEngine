using LuminaSharp;

namespace Lumina;

// The physics a system may use beside every other: queries and body reads now, body commands queued for the next step. Every query misses without a physics scene.
public readonly struct SystemPhysics
{
    private readonly SSystemContext Context;

    internal SystemPhysics(SSystemContext Context)
    {
        this.Context = Context;
    }

    public SRayResult Raycast(FVector3 Start, FVector3 End, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => CSystemPhysicsLibrary.Raycast(Context, Start, End, IgnoreEntity ?? FEntity.Null, LayerMask);

    public SRayResult[] RaycastAll(FVector3 Start, FVector3 End, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => CSystemPhysicsLibrary.RaycastAll(Context, Start, End, IgnoreEntity ?? FEntity.Null, LayerMask);

    public SRayResult RaycastEntity(FEntity Entity, FVector3 Start, FVector3 End)
        => CSystemPhysicsLibrary.RaycastEntity(Context, Entity, Start, End);

    public SRayResult ShapeCast(SQueryShape Shape, FVector3 Start, FVector3 End, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => CSystemPhysicsLibrary.ShapeCast(Context, Shape, Start, End, IgnoreEntity ?? FEntity.Null, LayerMask);

    public SRayResult[] ShapeCastAll(SQueryShape Shape, FVector3 Start, FVector3 End, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => CSystemPhysicsLibrary.ShapeCastAll(Context, Shape, Start, End, IgnoreEntity ?? FEntity.Null, LayerMask);

    public SRayResult SphereCast(float Radius, FVector3 Start, FVector3 End, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => ShapeCast(Sphere(Radius), Start, End, IgnoreEntity, LayerMask);

    public FEntity[] Overlap(SQueryShape Shape, FVector3 Center, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => CSystemPhysicsLibrary.Overlap(Context, Shape, Center, IgnoreEntity ?? FEntity.Null, LayerMask);

    public FEntity[] OverlapSphere(FVector3 Center, float Radius, FEntity? IgnoreEntity = null, ECollisionProfiles LayerMask = ECollisionProfiles.All)
        => Overlap(Sphere(Radius), Center, IgnoreEntity, LayerMask);

    public bool IsAwake(FEntity Entity) => CSystemPhysicsLibrary.IsAwake(Context, Entity);

    // Bodies the solver is simulating; one call, rather than IsAwake on every body.
    public int GetAwakeBodyCount() => CSystemPhysicsLibrary.GetAwakeBodyCount(Context);

    public FVector3 GetBodyPosition(FEntity Entity) => CSystemPhysicsLibrary.GetBodyPosition(Context, Entity);

    public FQuat GetBodyRotation(FEntity Entity) => CSystemPhysicsLibrary.GetBodyRotation(Context, Entity);

    public FVector3 GetLinearVelocity(FEntity Entity) => CSystemPhysicsLibrary.GetLinearVelocity(Context, Entity);

    public FVector3 GetAngularVelocity(FEntity Entity) => CSystemPhysicsLibrary.GetAngularVelocity(Context, Entity);

    public FVector3 GetVelocityAtPoint(FEntity Entity, FVector3 Point) => CSystemPhysicsLibrary.GetVelocityAtPoint(Context, Entity, Point);

    public FVector3 GetCenterOfMass(FEntity Entity) => CSystemPhysicsLibrary.GetCenterOfMass(Context, Entity);

    public float GetMass(FEntity Entity) => CSystemPhysicsLibrary.GetMass(Context, Entity);

    public SAABB GetBodyBounds(FEntity Entity) => CSystemPhysicsLibrary.GetBodyBounds(Context, Entity);

    public void AddForce(FEntity Entity, FVector3 Force) => CSystemPhysicsLibrary.AddForce(Context, Entity, Force);

    public void AddImpulse(FEntity Entity, FVector3 Impulse) => CSystemPhysicsLibrary.AddImpulse(Context, Entity, Impulse);

    public void AddTorque(FEntity Entity, FVector3 Torque) => CSystemPhysicsLibrary.AddTorque(Context, Entity, Torque);

    public void AddAngularImpulse(FEntity Entity, FVector3 AngularImpulse) => CSystemPhysicsLibrary.AddAngularImpulse(Context, Entity, AngularImpulse);

    public void AddForceAtPosition(FEntity Entity, FVector3 Force, FVector3 Position) => CSystemPhysicsLibrary.AddForceAtPosition(Context, Entity, Force, Position);

    public void AddImpulseAtPosition(FEntity Entity, FVector3 Impulse, FVector3 Position) => CSystemPhysicsLibrary.AddImpulseAtPosition(Context, Entity, Impulse, Position);

    public void SetLinearVelocity(FEntity Entity, FVector3 Velocity) => CSystemPhysicsLibrary.SetLinearVelocity(Context, Entity, Velocity);

    public void SetAngularVelocity(FEntity Entity, FVector3 Velocity) => CSystemPhysicsLibrary.SetAngularVelocity(Context, Entity, Velocity);

    public void SetGravityFactor(FEntity Entity, float Factor) => CSystemPhysicsLibrary.SetGravityFactor(Context, Entity, Factor);

    private static SQueryShape Sphere(float Radius) => new() { Type = EQueryShapeType.Sphere, Radius = Radius, Rotation = FQuat.Identity };
}
