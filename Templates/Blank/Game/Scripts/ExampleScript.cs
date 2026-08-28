using System;
using LuminaSharp;
using Lumina;

namespace GameScripts;

/// <summary>
/// Starter C# entity script. Add a "C# Script" component to an entity in your scene and select this
/// script, then press Play. Scripts compile inside the editor on save -- no rebuild, no restart.
///
/// EntityScript gives you: Entity, World, Registry, the cached Transform, and lifecycle hooks
/// (OnAttach / OnReady / OnUpdate / OnDetach), plus input and collision callbacks.
/// </summary>
public sealed class ExampleScript : EntityScript
{
    [Property(Tooltip = "Degrees per second to spin around the up (Y) axis.")]
    public float SpinSpeed = 90.0f;

    public override void OnReady()
    {
        Debug.Log($"ExampleScript ready on entity {Entity.Id}.");
    }

    public override void OnUpdate(float DeltaTime)
    {
        // Spin the entity. Transform is this entity's STransformComponent, resolved once and cached.
        Transform.AddLocalRotationFromEuler(new FVector3(0.0f, SpinSpeed * DeltaTime, 0.0f));
    }
}
