using System;
using LuminaSharp;
using Lumina;

namespace Game;

public sealed class NewScript : EntityScript
{
    [Property] public Entity TargetEntity;

    [Property] public uint ToSpawn = 9000;
    [Property] public uint Chunk = 10;

    public override void OnReady()
    {
    }

    public override void OnUpdate(float deltaTime)
    {
        if (World.GetNumEntities() >= ToSpawn) return;
        
        for (var i = 0; i < Chunk; i++)
        {
            var newEntity = World.DuplicateEntity(TargetEntity);
            World.SetEntityLocation(newEntity, new FVector3(Random.Shared.Next(-1000, 1000), Random.Shared.Next(-100, 100), Random.Shared.Next(-1000, 1000)));
        }
    }
}
