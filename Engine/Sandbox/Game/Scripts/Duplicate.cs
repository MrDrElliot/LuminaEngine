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
        for (int i = 0; i < 10; i++)
        {
            var NewEntity = World.DuplicateEntity(TargetEntity);
            World.SetEntityLocation(NewEntity, new FVector3(Random.Shared.Next(-100, 100), Random.Shared.Next(-100, 100), Random.Shared.Next(-100, 100)));   
        }
    }
}
