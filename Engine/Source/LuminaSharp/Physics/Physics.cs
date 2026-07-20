using System;
using Lumina;

namespace LuminaSharp;

/// A world's physics interface (World.Physics), keyed by Entity. Operations on an entity without a physics body are safe (mutators no-op, getters return zero). Game thread only.
public readonly unsafe partial struct Physics
{
    internal readonly ulong Handle;

    internal Physics(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    /// Casts a ray from Origin along Direction (normalized internally) for Distance; returns the closest hit or null. Pass Ignore to exclude one entity's body.
    public RaycastHit? Raycast(FVector3 Origin, FVector3 Direction, float Distance, Entity? Ignore = null)
    {
        FVector3 End = Origin + Direction.Normalized() * Distance;
        uint IgnoreId = Ignore?.Id ?? Entity.Null.Id;
        RaycastHitWire Result = RaycastWire(Origin, End, IgnoreId);
        if (Result.Hit == 0)
        {
            return null;
        }
        return new RaycastHit(new Entity(Result.Entity), Result.BodyId, Result.Point, Result.Normal, Result.Distance, Result.Fraction, Result.BoneIndex);
    }

    /// Largest overlap/sweep result set a single allocating query returns (extras are dropped).
    public const int MaxQueryResults = 256;

    /// All distinct entities whose bodies overlap a sphere at Center. Pass Ignore to exclude one entity. Capped at MaxQueryResults.
    public Entity[] OverlapSphere(FVector3 Center, float Radius, Entity? Ignore = null)
    {
        Span<uint> Buffer = stackalloc uint[MaxQueryResults];
        int Count = OverlapSphereRaw(Center, Radius, Ignore?.Id ?? Entity.Null.Id, Buffer);
        return ToEntities(Buffer, Count);
    }

    /// Writes overlapping entities into a caller buffer; returns the count written. Allocation-free.
    public int OverlapSphere(FVector3 Center, float Radius, Span<uint> Results, Entity? Ignore = null)
        => OverlapSphereRaw(Center, Radius, Ignore?.Id ?? Entity.Null.Id, Results);

    /// All distinct entities whose bodies overlap an oriented box. See OverlapSphere.
    public Entity[] OverlapBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, Entity? Ignore = null)
    {
        Span<uint> Buffer = stackalloc uint[MaxQueryResults];
        int Count = OverlapBoxRaw(Center, HalfExtents, Rotation, Ignore?.Id ?? Entity.Null.Id, Buffer);
        return ToEntities(Buffer, Count);
    }

    /// Axis-aligned box overlap (identity rotation).
    public Entity[] OverlapBox(FVector3 Center, FVector3 HalfExtents, Entity? Ignore = null)
        => OverlapBox(Center, HalfExtents, FQuat.Identity, Ignore);

    /// Sweeps a sphere from Origin along Direction for Distance; returns every hit sorted near-to-far (thick raycast). Pass Ignore to exclude one entity's body.
    public RaycastHit[] SphereCast(FVector3 Origin, FVector3 Direction, float Distance, float Radius, Entity? Ignore = null)
    {
        FVector3 End = Origin + Direction.Normalized() * Distance;
        Span<RaycastHitWire> Buffer = stackalloc RaycastHitWire[MaxQueryResults];
        int Count = SphereCastRaw(Origin, End, Radius, Ignore?.Id ?? Entity.Null.Id, Buffer);

        RaycastHit[] Out = new RaycastHit[Count];
        for (int i = 0; i < Count; ++i)
        {
            RaycastHitWire W = Buffer[i];
            Out[i] = new RaycastHit(new Entity(W.Entity), W.BodyId, W.Point, W.Normal, W.Distance, W.Fraction, W.BoneIndex);
        }
        return Out;
    }

    /// Every body a ray crosses, sorted near-to-far (penetrating line trace); one entry per body. Pass Ignore to exclude one entity's body. Capped at MaxQueryResults.
    public RaycastHit[] RaycastAll(FVector3 Origin, FVector3 Direction, float Distance, Entity? Ignore = null)
    {
        FVector3 End = Origin + Direction.Normalized() * Distance;
        Span<RaycastHitWire> Buffer = stackalloc RaycastHitWire[MaxQueryResults];
        int Count = RaycastAllRaw(Origin, End, Ignore?.Id ?? Entity.Null.Id, Buffer);

        RaycastHit[] Out = new RaycastHit[Count];
        for (int i = 0; i < Count; ++i)
        {
            RaycastHitWire W = Buffer[i];
            Out[i] = new RaycastHit(new Entity(W.Entity), W.BodyId, W.Point, W.Normal, W.Distance, W.Fraction, W.BoneIndex);
        }
        return Out;
    }

    /// Closest hit, restricted to bodies whose collision layer intersects Mask.
    public RaycastHit? RaycastFiltered(FVector3 Origin, FVector3 Direction, float Distance, ECollisionProfiles Mask, Entity? Ignore = null)
    {
        FVector3 End = Origin + Direction.Normalized() * Distance;
        RaycastHitWire Result = RaycastFilteredWire(Origin, End, Ignore?.Id ?? Entity.Null.Id, (uint)Mask);
        if (Result.Hit == 0)
        {
            return null;
        }
        return new RaycastHit(new Entity(Result.Entity), Result.BodyId, Result.Point, Result.Normal, Result.Distance, Result.Fraction, Result.BoneIndex);
    }

    /// Every hit near-to-far, restricted by collision layer mask. See RaycastAll.
    public RaycastHit[] RaycastAllFiltered(FVector3 Origin, FVector3 Direction, float Distance, ECollisionProfiles Mask, Entity? Ignore = null)
    {
        FVector3 End = Origin + Direction.Normalized() * Distance;
        Span<RaycastHitWire> Buffer = stackalloc RaycastHitWire[MaxQueryResults];
        int Count = RaycastAllFilteredRaw(Origin, End, Ignore?.Id ?? Entity.Null.Id, (uint)Mask, Buffer);

        RaycastHit[] Out = new RaycastHit[Count];
        for (int i = 0; i < Count; ++i)
        {
            RaycastHitWire W = Buffer[i];
            Out[i] = new RaycastHit(new Entity(W.Entity), W.BodyId, W.Point, W.Normal, W.Distance, W.Fraction, W.BoneIndex);
        }
        return Out;
    }

    /// All distinct entities whose bodies CONTAIN Point (volume containment, no shape sweep). Pass Ignore to exclude one entity. Capped at MaxQueryResults.
    public Entity[] OverlapPoint(FVector3 Point, Entity? Ignore = null)
    {
        Span<uint> Buffer = stackalloc uint[MaxQueryResults];
        int Count = OverlapPointRaw(Point, Ignore?.Id ?? Entity.Null.Id, Buffer);
        return ToEntities(Buffer, Count);
    }

    private static Entity[] ToEntities(Span<uint> Ids, int Count)
    {
        Entity[] Out = new Entity[Count];
        for (int i = 0; i < Count; ++i)
        {
            Out[i] = new Entity(Ids[i]);
        }
        return Out;
    }

    public void AddForce(Entity Entity, FVector3 Force) => AddForce(Entity.Id, Force);
    public void AddImpulse(Entity Entity, FVector3 Impulse) => AddImpulse(Entity.Id, Impulse);
    public void AddTorque(Entity Entity, FVector3 Torque) => AddTorque(Entity.Id, Torque);
    public void AddAngularImpulse(Entity Entity, FVector3 Impulse) => AddAngularImpulse(Entity.Id, Impulse);
    public void AddForceAtPosition(Entity Entity, FVector3 Force, FVector3 Position) => AddForceAtPosition(Entity.Id, Force, Position);
    public void AddImpulseAtPosition(Entity Entity, FVector3 Impulse, FVector3 Position) => AddImpulseAtPosition(Entity.Id, Impulse, Position);

    public FVector3 GetLinearVelocity(Entity Entity) => GetLinearVelocity(Entity.Id);
    public void SetLinearVelocity(Entity Entity, FVector3 Velocity) => SetLinearVelocity(Entity.Id, Velocity);
    public FVector3 GetAngularVelocity(Entity Entity) => GetAngularVelocity(Entity.Id);
    public void SetAngularVelocity(Entity Entity, FVector3 Velocity) => SetAngularVelocity(Entity.Id, Velocity);
    public FVector3 GetVelocityAtPoint(Entity Entity, FVector3 Point) => GetVelocityAtPoint(Entity.Id, Point);

    public FVector3 GetBodyPosition(Entity Entity) => GetBodyPosition(Entity.Id);
    public FQuat GetBodyRotation(Entity Entity) => GetBodyRotation(Entity.Id);
    public FVector3 GetCenterOfMass(Entity Entity) => GetCenterOfMass(Entity.Id);
    public void SetGravityFactor(Entity Entity, float Factor) => SetGravityFactor(Entity.Id, Factor);
    public void ActivateBody(Entity Entity) => ActivateBody(Entity.Id);
    public void DeactivateBody(Entity Entity) => DeactivateBody(Entity.Id);

    /// The entity's native body id, or 0xFFFFFFFF if it has no rigid body.
    public uint GetBodyId(Entity Entity) => GetBodyId(Entity.Id);

    /// True if the entity's body is awake (active); false if asleep or it has no body. Cheap to poll each frame.
    public bool IsAwake(Entity Entity) => IsAwakeRaw(Entity.Id) != 0;

    // Constraints / joints. Each Create* returns an FPhysicsConstraint handle; pass null for one side to anchor to the world. Frames are world-space; BreakForce > 0 disables the joint past that many N.

    /// Welds two bodies (or a body to the world) rigidly at their current relative pose.
    public FPhysicsConstraint CreateFixed(Entity? BodyA, Entity? BodyB, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypeFixed, BodyA, BodyB);
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Ball-and-socket joint pinned at Pivot (3 translation DOF removed).
    public FPhysicsConstraint CreatePoint(Entity? BodyA, Entity? BodyB, FVector3 Pivot, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypePoint, BodyA, BodyB);
        W.Anchor = Pivot;
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Keeps PointA on body A and PointB on body B between MinDistance..MaxDistance apart (negative = current distance); positive Frequency makes the limit a soft spring.
    public FPhysicsConstraint CreateDistance(Entity? BodyA, Entity? BodyB, FVector3 PointA, FVector3 PointB,
        float MinDistance = -1f, float MaxDistance = -1f, float Frequency = 0f, float Damping = 0f, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypeDistance, BodyA, BodyB);
        W.Anchor = PointA;
        W.AnchorB = PointB;
        W.MinLimit = MinDistance;
        W.MaxLimit = MaxDistance;
        W.HasLimits = (MinDistance >= 0f || MaxDistance >= 0f) ? 1 : 0;
        W.LimitFrequency = Frequency;
        W.LimitDamping = Damping;
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Single-axis hinge at Pivot about Axis; set Limited with min/max angles (radians) to clamp the swing, and MotorTorqueLimit to power it.
    public FPhysicsConstraint CreateHinge(Entity? BodyA, Entity? BodyB, FVector3 Pivot, FVector3 Axis,
        float MinAngle = 0f, float MaxAngle = 0f, bool Limited = false, float MaxFrictionTorque = 0f,
        float MotorTorqueLimit = 0f, float MotorFrequency = 0f, float MotorDamping = 0f, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypeHinge, BodyA, BodyB);
        W.Anchor = Pivot;
        W.Axis = Axis;
        W.MinLimit = MinAngle;
        W.MaxLimit = MaxAngle;
        W.HasLimits = Limited ? 1 : 0;
        W.MaxFriction = MaxFrictionTorque;
        W.MotorTorqueLimit = MotorTorqueLimit;
        W.MotorFrequency = MotorFrequency;
        W.MotorDamping = MotorDamping;
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Prismatic (slider) joint allowing motion along Axis only; set Limited with min/max positions (meters) to clamp travel, and MotorForceLimit to power it.
    public FPhysicsConstraint CreateSlider(Entity? BodyA, Entity? BodyB, FVector3 Pivot, FVector3 Axis,
        float MinDistance = 0f, float MaxDistance = 0f, bool Limited = false, float MaxFrictionForce = 0f,
        float MotorForceLimit = 0f, float MotorFrequency = 0f, float MotorDamping = 0f, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypeSlider, BodyA, BodyB);
        W.Anchor = Pivot;
        W.Axis = Axis;
        W.MinLimit = MinDistance;
        W.MaxLimit = MaxDistance;
        W.HasLimits = Limited ? 1 : 0;
        W.MaxFriction = MaxFrictionForce;
        W.MotorForceLimit = MotorForceLimit;
        W.MotorFrequency = MotorFrequency;
        W.MotorDamping = MotorDamping;
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Cone (swing-limited ball-socket): keeps body B's TwistAxis within HalfAngleRadians of body A's, pinned at Pivot.
    public FPhysicsConstraint CreateCone(Entity? BodyA, Entity? BodyB, FVector3 Pivot, FVector3 TwistAxis,
        float HalfAngleRadians, float BreakForce = 0f)
    {
        FConstraintDescWire W = NewDesc(FPhysicsConstraint.TypeCone, BodyA, BodyB);
        W.Anchor = Pivot;
        W.Axis = TwistAxis;
        W.HalfConeAngle = HalfAngleRadians;
        W.BreakForce = BreakForce;
        return new FPhysicsConstraint(Handle, CreateConstraintRaw(W));
    }

    /// Make this entity's collider act as a conveyor: bodies resting on it are dragged at Linear (world m/s). Needs a rigid body; zero clears it.
    public void SetSurfaceVelocity(Entity Entity, FVector3 Linear) => SetSurfaceVelocity(Entity.Id, Linear, default);

    /// Conveyor surface velocity with an angular component (rad/s about the body center).
    public void SetSurfaceVelocity(Entity Entity, FVector3 Linear, FVector3 Angular) => SetSurfaceVelocity(Entity.Id, Linear, Angular);

    /// Stop this entity behaving as a conveyor.
    public void ClearSurfaceVelocity(Entity Entity) => SetSurfaceVelocity(Entity.Id, default, default);

    private FConstraintDescWire NewDesc(int Type, Entity? BodyA, Entity? BodyB)
    {
        return new FConstraintDescWire
        {
            Type = Type,
            BodyA = BodyA?.Id ?? Entity.Null.Id,
            BodyB = BodyB?.Id ?? Entity.Null.Id,
            Axis = new FVector3(0f, 1f, 0f),
        };
    }

    // Flat shims (Runtime module). The world Handle is the first native argument; entities are entt ids (uint); vectors pass by value.

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_Raycast")]
    private partial RaycastHitWire RaycastWire(FVector3 Origin, FVector3 End, uint IgnoreId);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_OverlapSphere")]
    private partial int OverlapSphereRaw(FVector3 Center, float Radius, uint IgnoreId, Span<uint> Results);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_OverlapBox")]
    private partial int OverlapBoxRaw(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, uint IgnoreId, Span<uint> Results);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SphereCast")]
    private partial int SphereCastRaw(FVector3 Start, FVector3 End, float Radius, uint IgnoreId, Span<RaycastHitWire> Hits);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_RaycastAll")]
    private partial int RaycastAllRaw(FVector3 Start, FVector3 End, uint IgnoreId, Span<RaycastHitWire> Hits);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_RaycastFiltered")]
    private partial RaycastHitWire RaycastFilteredWire(FVector3 Start, FVector3 End, uint IgnoreId, uint LayerMask);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_RaycastAllFiltered")]
    private partial int RaycastAllFilteredRaw(FVector3 Start, FVector3 End, uint IgnoreId, uint LayerMask, Span<RaycastHitWire> Hits);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_OverlapPoint")]
    private partial int OverlapPointRaw(FVector3 Point, uint IgnoreId, Span<uint> Results);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddForce")]
    private partial void AddForce(uint Entity, FVector3 Force);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddImpulse")]
    private partial void AddImpulse(uint Entity, FVector3 Impulse);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddTorque")]
    private partial void AddTorque(uint Entity, FVector3 Torque);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddAngularImpulse")]
    private partial void AddAngularImpulse(uint Entity, FVector3 Impulse);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddForceAtPosition")]
    private partial void AddForceAtPosition(uint Entity, FVector3 Force, FVector3 Position);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_AddImpulseAtPosition")]
    private partial void AddImpulseAtPosition(uint Entity, FVector3 Impulse, FVector3 Position);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetLinearVelocity")]
    private partial FVector3 GetLinearVelocity(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetLinearVelocity")]
    private partial void SetLinearVelocity(uint Entity, FVector3 Velocity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetAngularVelocity")]
    private partial FVector3 GetAngularVelocity(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetAngularVelocity")]
    private partial void SetAngularVelocity(uint Entity, FVector3 Velocity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetVelocityAtPoint")]
    private partial FVector3 GetVelocityAtPoint(uint Entity, FVector3 Point);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetBodyPosition")]
    private partial FVector3 GetBodyPosition(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetBodyRotation")]
    private partial FQuat GetBodyRotation(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetCenterOfMass")]
    private partial FVector3 GetCenterOfMass(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetGravityFactor")]
    private partial void SetGravityFactor(uint Entity, float Factor);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_ActivateBody")]
    private partial void ActivateBody(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_DeactivateBody")]
    private partial void DeactivateBody(uint Entity);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetBodyId")]
    private partial uint GetBodyId(uint Entity);

    // Constraints. CreateConstraint takes the blittable FConstraintDescWire by value; the others key off the constraint id it returned.
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_CreateConstraint")]
    private partial uint CreateConstraintRaw(FConstraintDescWire Desc);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_DestroyConstraint")]
    internal partial void DestroyConstraintRaw(uint ConstraintId);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetConstraintEnabled")]
    internal partial void SetConstraintEnabledRaw(uint ConstraintId, int Enabled);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetConstraintMotor")]
    internal partial void SetConstraintMotorRaw(uint ConstraintId, int Mode, float Target);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_IsConstraintBroken")]
    internal partial int IsConstraintBrokenRaw(uint ConstraintId);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetConstraintValue")]
    internal partial float GetConstraintValueRaw(uint ConstraintId);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_IsAwake")]
    private partial int IsAwakeRaw(uint Entity);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_SetSurfaceVelocity")]
    private partial void SetSurfaceVelocity(uint Entity, FVector3 Linear, FVector3 Angular);
}
