namespace LuminaSharp;

// Shorthand for the entity a callback is working with, resolved against the callback's own world.
public static class EntityExtensions
{
    public static bool IsValid(this Entity Entity) => !Entity.IsNull && Engine.World.IsValidEntity(Entity);

    public static T Get<T>(this Entity Entity) where T : NativeStruct => Engine.World.Registry.Get<T>(Entity);

    public static T? TryGet<T>(this Entity Entity) where T : NativeStruct => Engine.World.Registry.TryGet<T>(Entity);

    public static bool Has<T>(this Entity Entity) where T : NativeStruct => Engine.World.Registry.Has<T>(Entity);

    public static T? GetOrAdd<T>(this Entity Entity) where T : NativeStruct => Engine.World.Registry.GetOrAdd<T>(Entity);

    public static T? GetScript<T>(this Entity Entity) where T : Lumina.CEntityScript => Engine.World.Registry.GetScript<T>(Entity);

    public static Lumina.FVector3 GetLocation(this Entity Entity) => Engine.World.GetEntityLocation(Entity);

    public static void SetLocation(this Entity Entity, Lumina.FVector3 Location) => Engine.World.SetEntityLocation(Entity, Location);

    // Destroys the entity and every child under it. A null or already destroyed entity is left alone.
    public static void Destroy(this Entity Entity)
    {
        if (Entity.IsValid())
        {
            Engine.World.DestroyEntity(Entity);
        }
    }
}
