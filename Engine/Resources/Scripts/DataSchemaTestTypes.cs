#nullable disable

using LuminaSharp;
using Lumina;

namespace Engine;

public enum EGuardStance
{
    Relaxed,
    Suspicious,
    Hostile,
}

/**
 * Exercises the blackboard backing-struct path from C#.
 *
 * Nothing references this type. That is the point: a marked type is discovered on its own pass over the
 * loaded assembly, so if it shows up in a blackboard asset's Backing Struct dropdown then discovery is
 * genuinely independent of field references.
 *
 * Covers one of every key type the mapper handles, so a wrong mapping shows up as a wrong key type rather
 * than as a missing key.
 */
[BlackboardData]
public struct GuardBrainData
{
    [Property] public float Health = 100.0f;        // -> Float, default 100
    [Property] public int Ammo = 30;                // -> Int, default 30
    [Property] public bool Alerted = false;         // -> Bool
    [Property] public EGuardStance Stance = EGuardStance.Relaxed;   // -> Enum, EnumType = EGuardStance
    [Property] public FVector3 LastKnownPosition = new FVector3 { X = 0.0f, Y = 0.0f, Z = 0.0f }; // -> Vector

    // A member the mapper has no key type for. Should be skipped silently rather than producing a
    // broken key: strings are not a blackboard value type.
    [Property] public string DebugLabel = "guard";

    public GuardBrainData() { }
}

/**
 * The same mechanism reached through the other marker, to prove both features share one path rather than
 * each having grown its own. Selectable as a data table's Row Struct.
 */
[DataTableRow]
public struct WeaponStatsRow
{
    [Property] public float Damage = 25.0f;
    [Property] public float FireRate = 0.15f;
    [Property] public int ClipSize = 12;
    [Property] public bool IsAutomatic = true;

    public WeaponStatsRow() { }
}
