using System;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// A hierarchical tag, e.g. <c>"Ability.Fire.Fireball"</c>. A lightweight value
/// wrapping an interned id from the process-global registry, so equality and hierarchical matching are
/// O(depth) integer compares. Request one with <see cref="Request"/>; the registry interns the tag and all
/// its ancestors. Used as message-bus channels (<see cref="Lumina.CWorld"/> Subscribe and Broadcast), entity/ability tags, etc.
/// </summary>
public readonly partial struct HierarchicalTag : IEquatable<HierarchicalTag>
{
    /// <summary>The interned registry id; 0 is <see cref="None"/>.</summary>
    public readonly uint Id;

    internal HierarchicalTag(uint Id)
    {
        this.Id = Id;
    }

    /// <summary>The invalid / empty tag.</summary>
    public static readonly HierarchicalTag None = default;

    /// <summary>Intern (or look up) a dotted tag name and its ancestors; returns <see cref="None"/> if empty.</summary>
    public static HierarchicalTag Request(string Name)
    {
        return string.IsNullOrEmpty(Name) ? None : new HierarchicalTag(CHierarchicalTagLibrary.RequestTag(Name));
    }

    public bool IsValid => Id != 0;

    /// <summary>The dotted tag name, or empty for <see cref="None"/>.</summary>
    public string Name => Id == 0 ? string.Empty : CHierarchicalTagLibrary.GetTagName(Id);

    /// <summary>The immediate parent tag (<c>"Ability.Fire.Fireball"</c> -> <c>"Ability.Fire"</c>), or None at the root.</summary>
    public HierarchicalTag Parent => new HierarchicalTag(CHierarchicalTagLibrary.GetParent(Id));

    /// <summary>
    /// Hierarchical match: true when this tag IS <paramref name="Other"/> or a descendant of it (this is at
    /// or below Other in the tree). So <c>Request("Ability.Fire.Fireball").Matches(Request("Ability.Fire"))</c>
    /// is true, but the reverse is false.
    /// </summary>
    public bool Matches(HierarchicalTag Other) => CHierarchicalTagLibrary.Matches(Id, Other.Id);

    /// <summary>Exact (non-hierarchical) tag equality.</summary>
    public bool MatchesExact(HierarchicalTag Other) => Id != 0 && Id == Other.Id;

    public bool Equals(HierarchicalTag Other) => Id == Other.Id;
    public override bool Equals(object? Obj) => Obj is HierarchicalTag Tag && Tag.Id == Id;
    public override int GetHashCode() => (int)Id;
    public static bool operator ==(HierarchicalTag A, HierarchicalTag B) => A.Id == B.Id;
    public static bool operator !=(HierarchicalTag A, HierarchicalTag B) => A.Id != B.Id;
    public override string ToString() => Id == 0 ? "HierarchicalTag.None" : Name;
}
