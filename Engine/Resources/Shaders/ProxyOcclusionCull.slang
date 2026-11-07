#include "Includes/AppendBuffer.slang"
#include "Includes/VisBufferSurface.slang"

// Keeps the far proxies a two-phase camera can still see past this frame's early depth, so the proxy pass splats only those.

struct FProxyOcclusionCullArgs
{
    GPUSpan<uint> ProxyList;
    // The second word counts ProxyList and can run past its capacity.
    GPUSpan<uint> ProxyDrawArgs;
    GPUSpan<uint> OutProxyList;
    GPUSpan<uint> OutProxyDrawArgs;
    GPUSpan<uint> OutDispatchArgs;
};
FProxyOcclusionCullArgs* Pass() { return GetPassArgs<FProxyOcclusionCullArgs>(); }

uint ListedProxyCount()
{
    return Pass().ProxyDrawArgs.Count < 2u ? 0u : min(Pass().ProxyDrawArgs.Get(1u), Pass().ProxyList.Count);
}

#ifdef PROXY_CULL_ARGS

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    // Folded across Y exactly as every other oversized grid in the renderer; the cull entry undoes it.
    const uint Groups = (ListedProxyCount() + 63u) / 64u;
    Pass().OutDispatchArgs.Set(0u, min(Groups, (uint)MAX_DISPATCH_AXIS));
    Pass().OutDispatchArgs.Set(1u, (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS);
    Pass().OutDispatchArgs.Set(2u, 1u);
}

#else

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint Index = (GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS) * 64u + LocalID.x;
    if (Index >= ListedProxyCount())
    {
        return;
    }

    const uint Slot = Pass().ProxyList.Get(Index);
    if (!RetainedTransforms().Contains(Slot) || !RetainedProxies().Contains(Slot))
    {
        return;
    }

    const FTransform3x4  Transform = RetainedTransforms().Get(Slot);
    const FInstanceProxy Proxy     = RetainedProxies().GetAligned(Slot);

    // Bounds the quad ProxyVisBufferVert draws, whose corners sit a diagonal from the center.
    float3 GroundWiden;
    const float3 Center = ProxyWidenedCenter(Transform.ToMatrix(), Proxy, GroundWiden);
    const float  Radius = Proxy.SplatHalfSize * TransformMaxScale(Transform) * GroundWiden.x * sqrt(2.0);

    const FCullSphereProjection Proj = ProjectCullSphere(Center, Radius);
    if (Proj.bValid && IsHiZOccluded(Proj))
    {
        return;
    }

    const uint OutSlot = AppendReserve(Pass().OutProxyDrawArgs.At(1u), Pass().OutProxyList.Count, 0u);
    if (OutSlot != kAppendFull)
    {
        Pass().OutProxyList.Set(OutSlot, Slot);
    }
}

#endif
