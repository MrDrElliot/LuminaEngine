using System;
using Lumina;

namespace LuminaSharp;

// A dotted tag such as Ability.Fire.Fireball, an interned id so matching is a few integer compares.
public readonly partial struct HierarchicalTag : IEquatable<HierarchicalTag>
{
    // 0 is None.
    public readonly uint Id;

    internal HierarchicalTag(uint Id)
    {
        this.Id = Id;
    }

    public static readonly HierarchicalTag None = default;

    // Interns a dotted tag name and its ancestors, or returns None for an empty name.
    public static HierarchicalTag Request(string Name)
    {
        return string.IsNullOrEmpty(Name) ? None : new HierarchicalTag(CHierarchicalTagLibrary.RequestTag(Name));
    }

    public bool IsValid => Id != 0;

    public string Name => Id == 0 ? string.Empty : CHierarchicalTagLibrary.GetTagName(Id);

    // None at the root.
    public HierarchicalTag Parent => new HierarchicalTag(CHierarchicalTagLibrary.GetParent(Id));

    // True when this tag is Other or a descendant of it, so Ability.Fire.Fireball matches Ability.Fire but not the reverse.
    public bool Matches(HierarchicalTag Other) => CHierarchicalTagLibrary.Matches(Id, Other.Id);

    public bool MatchesExact(HierarchicalTag Other) => Id != 0 && Id == Other.Id;

    public bool Equals(HierarchicalTag Other) => Id == Other.Id;
    public override bool Equals(object? Obj) => Obj is HierarchicalTag Tag && Tag.Id == Id;
    public override int GetHashCode() => (int)Id;
    public static bool operator ==(HierarchicalTag A, HierarchicalTag B) => A.Id == B.Id;
    public static bool operator !=(HierarchicalTag A, HierarchicalTag B) => A.Id != B.Id;
    public override string ToString() => Id == 0 ? "HierarchicalTag.None" : Name;
}
