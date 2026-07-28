using System;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// A world's AI perception interface (<c>World.Perception</c>). Reads what an entity's
/// <c>SPerceptionComponent</c> currently senses (sight/hearing/damage) and injects event-driven stimuli
/// (noise, damage). Sight is scanned by <c>SPerceptionSystem</c> each tick; hearing/damage are reported on
/// demand. Scripts usually respond via the <c>EntityScript.OnTargetPerceived</c> / <c>OnTargetLost</c>
/// callbacks and use these queries for follow-up logic. Game thread only; every member forwards to a flat
/// <c>LuminaSharp_Perception_*</c> shim (DotNetPerception.cpp) with the world Handle passed first.
/// </summary>
public readonly unsafe partial struct Perception
{
    internal readonly ulong Handle;

    internal Perception(ulong Handle)
    {
        this.Handle = Handle;
    }

    /// <summary>Maximum targets a single perceiver tracks (matches SPerceptionComponent.MaxPerceivedTargets).</summary>
    public const int MaxPerceivedTargets = 16;

    public bool IsValid => Handle != 0;

    /// <summary>Writes the perceiver's currently-tracked targets into <paramref name="Buffer"/>; returns the
    /// count written. Allocation-free: pass a <c>stackalloc</c> span.</summary>
    public int GetPerceivedTargets(Entity Perceiver, Span<uint> Buffer)
        => GetPerceivedTargetsRaw(Perceiver.Id, Buffer);

    /// <summary>Allocating convenience: the perceiver's currently-tracked targets.</summary>
    public Entity[] GetPerceivedTargets(Entity Perceiver)
    {
        Span<uint> Buffer = stackalloc uint[MaxPerceivedTargets];
        int Count = GetPerceivedTargetsRaw(Perceiver.Id, Buffer);
        Entity[] Result = new Entity[Count];
        for (int i = 0; i < Count; ++i)
        {
            Result[i] = new Entity(Buffer[i]);
        }
        return Result;
    }

    /// <summary>True if <paramref name="Perceiver"/> currently senses <paramref name="Target"/> by any sense.</summary>
    public bool CanSense(Entity Perceiver, Entity Target)
        => CanSenseRaw(Perceiver.Id, Target.Id, (int)(EAISenseChannel.Sight | EAISenseChannel.Hearing | EAISenseChannel.Damage)) != 0;

    /// <summary>True if <paramref name="Perceiver"/> currently senses <paramref name="Target"/> via <paramref name="Sense"/>.</summary>
    public bool CanSense(Entity Perceiver, Entity Target, EAISenseChannel Sense)
        => CanSenseRaw(Perceiver.Id, Target.Id, (int)Sense) != 0;

    /// <summary>Last sensed location of <paramref name="Target"/> (held during the forget window), or null.</summary>
    public FVector3? GetLastKnownLocation(Entity Perceiver, Entity Target)
    {
        PerceptionPointWire Wire = GetLastKnownLocationRaw(Perceiver.Id, Target.Id);
        return Wire.Found != 0 ? Wire.Point : null;
    }

    /// <summary>Nearest perceived target to <paramref name="Perceiver"/>, or <see cref="Entity.Null"/>.</summary>
    public Entity GetClosestPerceivedTarget(Entity Perceiver)
        => new(GetClosestRaw(Perceiver.Id));

    /// <summary>One-shot line-of-sight test between two entities (eye/aim offsets applied when present).</summary>
    public bool HasLineOfSight(Entity From, Entity To)
        => HasLineOfSightRaw(From.Id, To.Id) != 0;

    /// <summary>Report a noise at <paramref name="Location"/>. Heard by perceivers within their hearing radius
    /// scaled by <paramref name="Loudness"/> that care about <paramref name="Instigator"/>'s affiliation.</summary>
    public void ReportNoise(FVector3 Location, float Loudness, Entity Instigator)
        => ReportNoiseRaw(Location, Loudness, Instigator.Id);

    /// <summary>Report damage: <paramref name="Victim"/> immediately perceives <paramref name="Instigator"/>.</summary>
    public void ReportDamage(Entity Victim, Entity Instigator, FVector3 HitLocation, float Amount)
        => ReportDamageRaw(Victim.Id, Instigator.Id, HitLocation, Amount);

    /// <summary>Make <paramref name="Entity"/> perceivable: ensures an <c>SAIStimuliSourceComponent</c>, sets
    /// its registered senses, and adds one affiliation tag. Author richer sets in the editor.</summary>
    public void RegisterAsSource(Entity Entity, GameplayTag Affiliation,
        EAISenseChannel Senses = EAISenseChannel.Sight | EAISenseChannel.Hearing)
        => RegisterSourceRaw(Entity.Id, Affiliation.Id, (int)Senses);

    /// <summary>Ensure an <c>SPerceptionComponent</c> on <paramref name="Perceiver"/> and add <paramref name="Tag"/>
    /// to its affiliation filter, so it only senses sources carrying a matching tag (empty filter = sense everyone).</summary>
    public void AddDetectableTag(Entity Perceiver, GameplayTag Tag)
        => AddDetectableTagRaw(Perceiver.Id, Tag.Id);

    // Flat shims (Runtime module). World Handle is the first native arg; the Span expands to (uint*, int).
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_GetPerceivedTargets")]
    private partial int GetPerceivedTargetsRaw(uint Perceiver, Span<uint> Out);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_CanSense")]
    private partial int CanSenseRaw(uint Perceiver, uint Target, int SenseBits);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_GetLastKnownLocation")]
    private partial PerceptionPointWire GetLastKnownLocationRaw(uint Perceiver, uint Target);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_GetClosest")]
    private partial uint GetClosestRaw(uint Perceiver);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_HasLineOfSight")]
    private partial int HasLineOfSightRaw(uint From, uint To);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_ReportNoise")]
    private partial void ReportNoiseRaw(FVector3 Location, float Loudness, uint Instigator);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_ReportDamage")]
    private partial void ReportDamageRaw(uint Victim, uint Instigator, FVector3 HitLocation, float Amount);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_RegisterSource")]
    private partial void RegisterSourceRaw(uint Entity, uint AffiliationTagId, int SenseBits);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Perception_AddDetectableTag")]
    private partial void AddDetectableTagRaw(uint Perceiver, uint TagId);
}

/// <summary>Blittable mirror of the native FLmPerceptionPoint; the GetLastKnownLocation thunk return.</summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("PerceptionPointWire")]
internal struct PerceptionPointWire
{
    public int Found;
    public FVector3 Point;
}
