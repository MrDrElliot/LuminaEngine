using System.ComponentModel;

namespace LuminaSharp;

public abstract partial class EntityScript
{
    // Who controls this script's entity, and whether this peer does.
    public EntityNetwork Network => new(World, Entity);

    // Another peer controls this entity, so input and simulation belong to it, not here.
    public bool IsProxy => Network.IsProxy;

    // Overridden by the script compiler for every class declaring an [Rpc] method. Returns false for an unknown id.
    [EditorBrowsable(EditorBrowsableState.Never)]
    protected virtual bool __RpcDispatch(uint RpcId, ref NetReader Reader) => false;

    internal bool InvokeRpc(uint RpcId, ref NetReader Reader) => __RpcDispatch(RpcId, ref Reader);
}
