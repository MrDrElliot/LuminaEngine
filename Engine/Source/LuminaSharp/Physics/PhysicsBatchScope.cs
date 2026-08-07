using System;

namespace LuminaSharp;

/// A scoped rigid-body creation batch, returned by World.Physics.Batch(). Every body created while it is
/// open is inserted into the broadphase in one pass when it is disposed, instead of one insert per body.
/// Bodies do not exist until the scope closes, so their BodyId / velocity / mass are only valid after it.
/// Game thread only; nests (an inner scope folds into the outer one).
public readonly struct FPhysicsBatchScope : IDisposable
{
    private readonly Physics Owner;

    internal FPhysicsBatchScope(Physics Owner)
    {
        this.Owner = Owner;
        Owner.BeginBodyBatch();
    }

    public void Dispose()
    {
        if (Owner.IsValid)
        {
            Owner.EndBodyBatch();
        }
    }
}
