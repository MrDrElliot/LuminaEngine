#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// One camera-facing quad per retained slot the cull listed as too small for its meshlets, stamped into the VisBuffer with the proxy bit.

struct FProxyVertexOutput
{
    nointerpolation uint VisID : VISID;
    float4 Position            : SV_Position;
};

[shader("vertex")]
FProxyVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FProxyVertexOutput Out;
    Out.VisID    = VISBUFFER_PROXY_BIT | (InstanceIndex + 1u);
    Out.Position = float4(0.0, 0.0, 0.0, 0.0);

    if (!ProxyList().Contains(InstanceIndex))
    {
        return Out;
    }
    const uint Slot = ProxyList().Get(InstanceIndex);
    if (!RetainedTransforms().Contains(Slot) || !RetainedProxies().Contains(Slot))
    {
        return Out;
    }

    const FTransform3x4  Transform = RetainedTransforms().Get(Slot);
    const FInstanceProxy Proxy     = RetainedProxies().GetAligned(Slot);

    float3 GroundWiden;
    const float3 Center   = ProxyWidenedCenter(Transform.ToMatrix(), Proxy, GroundWiden);
    const float  HalfSize = Proxy.SplatHalfSize * TransformMaxScale(Transform);

    // Two triangles, corners 0 1 2 and 0 2 3 of a quad facing the camera.
    static const float2 Corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const float2 C = Corners[min(VertexIndex, 5u)];
    const float3 WorldPos = Center + (GetCameraRight() * C.x + GetCameraUp() * C.y) * HalfSize * GroundWiden;

    Out.Position = mul(GetCameraProjection(), mul(GetCameraView(), float4(WorldPos, 1.0)));
    return Out;
}
