using System;

namespace LuminaSharp;

/// <summary>
// A handle to an entity, mirroring C++ ECS::FEntity. Resolve components through EntityRegistry.
/// </summary>
public readonly struct Entity : IEquatable<Entity>
{
    /// <summary>The packed entity handle.</summary>
    public readonly uint Id;

    public Entity(uint Id)
    {
        this.Id = Id;
    }

    /// <summary>ECS::NullEntity sentinel (all bits set for the default 32-bit entity type).</summary>
    public static readonly Entity Null = new(0xFFFFFFFFu);

    public bool IsNull => Id == Null.Id;

    public bool Equals(Entity Other)
    {
        return Id == Other.Id;
    }

    public override bool Equals(object? Obj)
    {
        return Obj is Entity Other && Equals(Other);
    }

    public override int GetHashCode()
    {
        return (int)Id;
    }

    public static bool operator ==(Entity Left, Entity Right)
    {
        return Left.Id == Right.Id;
    }

    public static bool operator !=(Entity Left, Entity Right)
    {
        return Left.Id != Right.Id;
    }

    public override string ToString()
    {
        return IsNull ? "Entity(null)" : $"Entity({Id})";
    }
}
