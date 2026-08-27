using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// A live joint connecting two rigid bodies (or a body to the world), created by the <c>World.Physics</c>
/// <c>Create*</c> methods. Use it to drive a Hinge/Slider motor, query whether a breakable joint has
/// snapped, read its current value, toggle it, or tear it down. The handle is a lightweight value (world +
/// native id); it is safe to copy and store. Game thread only.
/// </summary>
public readonly struct FPhysicsConstraint
{
    // Wire contract with native EPhysicsConstraintType / the FConstraintDesc switch (DotNetGameplay.cpp).
    internal const int TypeFixed = 0;
    internal const int TypePoint = 1;
    internal const int TypeDistance = 2;
    internal const int TypeHinge = 3;
    internal const int TypeSlider = 4;
    internal const int TypeCone = 5;

    private readonly ulong World;

    /// <summary>Opaque native handle. 0 means creation failed (e.g. neither body had a rigid body).</summary>
    public readonly uint Id;

    internal FPhysicsConstraint(ulong World, uint Id)
    {
        this.World = World;
        this.Id = Id;
    }

    /// <summary>False when the joint failed to create. Operations on an invalid handle are safe no-ops.</summary>
    public bool IsValid => Id != 0;

    /// <summary>Removes the joint from the simulation. The handle is invalid afterwards.</summary>
    public void Destroy()
    {
        if (Id != 0)
        {
            new Physics(World).DestroyConstraintRaw(Id);
        }
    }

    /// <summary>Enable or disable the joint without destroying it (re-enabling clears a broken flag).</summary>
    public void SetEnabled(bool Enabled)
    {
        if (Id != 0)
        {
            new Physics(World).SetConstraintEnabledRaw(Id, Enabled ? 1 : 0);
        }
    }

    /// <summary>Drive a Hinge/Slider motor toward a target velocity (rad/s for a hinge, m/s for a slider).
    /// No effect on jointless types. Needs a force/torque limit set at creation to be useful.</summary>
    public void DriveToVelocity(float Target)
    {
        if (Id != 0)
        {
            new Physics(World).SetConstraintMotorRaw(Id, 1, Target);
        }
    }

    /// <summary>Drive a Hinge/Slider motor toward a target position (radians for a hinge, meters for a
    /// slider) using its motor spring. No effect on jointless types.</summary>
    public void DriveToPosition(float Target)
    {
        if (Id != 0)
        {
            new Physics(World).SetConstraintMotorRaw(Id, 2, Target);
        }
    }

    /// <summary>Turn the motor off (the joint goes back to free / friction-only).</summary>
    public void DisableMotor()
    {
        if (Id != 0)
        {
            new Physics(World).SetConstraintMotorRaw(Id, 0, 0f);
        }
    }

    /// <summary>True once a breakable joint exceeded its break force and was auto-disabled.</summary>
    public bool IsBroken => Id != 0 && new Physics(World).IsConstraintBrokenRaw(Id) != 0;

    /// <summary>The joint's current value: a Hinge's angle (radians) or a Slider's position (meters) -- read
    /// "how far open / how high" while you drive it. 0 for joint types without a single driven scalar.</summary>
    public float CurrentValue => Id != 0 ? new Physics(World).GetConstraintValueRaw(Id) : 0f;
}

/// <summary>Blittable mirror of the native FLmConstraintDesc (DotNetGameplay.cpp); the wire form of a
/// constraint creation request. Built by the <c>World.Physics.Create*</c> helpers, not used directly.</summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FConstraintDescWire")]
internal struct FConstraintDescWire
{
    public int Type;            // FPhysicsConstraint.Type* constant.
    public uint BodyA;          // entity id, 0xFFFFFFFF == world anchor.
    public uint BodyB;
    public FVector3 Anchor;     // World pivot (Point/Hinge/Slider/Cone) or first attach point (Distance).
    public FVector3 Axis;       // Hinge/Cone axis or Slider direction (world).
    public FVector3 AnchorB;    // Distance: second attach point.
    public float MinLimit;
    public float MaxLimit;
    public float HalfConeAngle;
    public int HasLimits;
    public float LimitFrequency;
    public float LimitDamping;
    public float MaxFriction;
    public float MotorFrequency;
    public float MotorDamping;
    public float MotorForceLimit;
    public float MotorTorqueLimit;
    public float BreakForce;
}
