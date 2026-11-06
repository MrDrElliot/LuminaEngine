#include "Includes/SceneGlobals.slang"
#include "Includes/Culling.slang"

// One camera-facing quad per retained slot the cull listed as too small for its meshlets, stamped into the VisBuffer with the proxy bit.

struct FProxyVertexOutput
{
    nointerpolation uint VisID : VISID;
    float4 Position            : SV_Position;
};

// A quad narrower than a pixel covers a center only some frames, which is the coverage a sub-pixel object has.
static const float kMinProxyPixels = 0.0;

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
    const float4x4       Model     = Transform.ToMatrix();

    const float3 Center = mul(Model, float4(Proxy.LocalCenter, 1.0)).xyz;
    const float  Radius = Proxy.LocalRadius * TransformMaxScale(Transform);

    const float4x4 Proj   = GetCameraProjection();
    const float    ViewZ  = mul(GetCameraView(), float4(Center, 1.0)).z;
    const float    Height = float(GetScreenSize().y);
    const float    PixelWorld = IsCameraOrthographic()
                              ? 2.0 / max(abs(Proj[1][1]) * Height, 1e-6)
                              : 2.0 * max(ViewZ, 1e-4) / max(abs(Proj[1][1]) * Height, 1e-6);
    const float HalfSize = max(Radius, PixelWorld * kMinProxyPixels * 0.5);

    // Two triangles, corners 0 1 2 and 0 2 3 of a quad facing the camera.
    static const float2 Corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const float2 C = Corners[min(VertexIndex, 5u)];
    const float3 WorldPos = Center + (GetCameraRight() * C.x + GetCameraUp() * C.y) * HalfSize;

    Out.Position = mul(Proj, mul(GetCameraView(), float4(WorldPos, 1.0)));
    return Out;
}
