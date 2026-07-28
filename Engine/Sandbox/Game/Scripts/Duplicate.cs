using System;
using LuminaSharp;
using Lumina;

namespace Game;

public sealed class NewScript : EntityScript
{
    [Property] public Entity TargetEntity;

    public override void OnReady()
    {
    }

    public override void OnUpdate(float deltaTime)
    {
        if (World.GetNumEntities() >= 10000) return;
        
        for (var i = 0; i < 1000; i++)
        {
            var newEntity = World.DuplicateEntity(TargetEntity);
            World.SetEntityLocation(newEntity, new FVector3(Random.Shared.Next(-100, 100), 0, Random.Shared.Next(-100, 100)));
        }
    }
}
