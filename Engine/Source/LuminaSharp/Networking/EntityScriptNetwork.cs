namespace LuminaSharp;

public abstract partial class EntityScript
{
    // Who controls this script's entity, and whether this peer does.
    public EntityNetwork Network => new(World, Entity);

    // Another peer controls this entity, so input and simulation belong to it, not here.
    public bool IsProxy => Network.IsProxy;
}
