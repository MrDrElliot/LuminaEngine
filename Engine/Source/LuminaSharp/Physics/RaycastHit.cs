using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// The result of a physics ray query (<see cref="Physics.Raycast"/>). Returned only on a hit, so a
/// non-null result always describes a real intersection.
/// </summary>
public readonly struct RaycastHit
{
    /// <summary>The entity that owns the hit body.</summary>
    public readonly Entity Entity;

    /// <summary>The hit body's native physics id.</summary>
    public readonly long BodyId;

    /// <summary>World-space hit point.</summary>
    public readonly FVector3 Point;

    /// <summary>World-space surface normal at the hit.</summary>
    public readonly FVector3 Normal;

    /// <summary>Distance from the ray origin to the hit (world units).</summary>
    public readonly float Distance;

    /// <summary>Normalized distance along the ray, 0 (origin) to 1 (end).</summary>
    public readonly float Fraction;

    /// <summary>
    /// Skeleton bone index behind the hit body, or -1. Only ragdoll per-bone bodies report a bone;
    /// resolve the name with <see cref="Lumina.CWorld.GetBoneName"/>, or use
    /// <see cref="Lumina.CWorld.FindClosestBone"/> at <see cref="Point"/> for single-body skeletal meshes.
    /// </summary>
    public readonly int BoneIndex;

    /// <summary>True when the hit body maps to a skeleton bone (see <see cref="BoneIndex"/>).</summary>
    public bool HasBone => BoneIndex >= 0;

    internal RaycastHit(Entity Entity, long BodyId, FVector3 Point, FVector3 Normal, float Distance, float Fraction, int BoneIndex)
    {
        this.Entity = Entity;
        this.BodyId = BodyId;
        this.Point = Point;
        this.Normal = Normal;
        this.Distance = Distance;
        this.Fraction = Fraction;
        this.BoneIndex = BoneIndex;
    }
}

/// <summary>Blittable mirror of the native FLmRayHit (DotNetGameplay.cpp); the raycast thunk's ABI return.</summary>
[NativeLayout("RaycastHitWire")]
[StructLayout(LayoutKind.Sequential)]
internal struct RaycastHitWire
{
    public int Hit;
    public uint Entity;
    public long BodyId;
    public FVector3 Point;
    public FVector3 Normal;
    public float Distance;
    public float Fraction;
    public int BoneIndex;
}
