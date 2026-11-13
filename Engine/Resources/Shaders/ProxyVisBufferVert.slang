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

    // A strip's two triangles, so the quad's shared corners are emitted once.
    static const float2 Corners[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    const float2 C = Corners[min(VertexIndex, 3u)];

    // Clip is linear in position, so the corner offsets project once as directions and add to the center's image.
    const float4x4 Projection  = GetCameraProjection();
    const float4   ClipCenter  = WorldToClip(Center);
    const float4   ClipRight   = mul(Projection, float4(NormalWorldToView(GetCameraRight() * HalfSize * GroundWiden), 0.0));
    const float4   ClipUp      = mul(Projection, float4(NormalWorldToView(GetCameraUp() * HalfSize * GroundWiden), 0.0));

    const float2 ScreenSize   = float2(GetScreenSize());
    const float2 ClipToPixels = ScreenSize * 0.5 / ClipCenter.w;
    const float2 RightPixels  = ClipRight.xy * ClipToPixels;
    const float2 UpPixels     = ClipUp.xy * ClipToPixels;

    // A splat narrower than a pixel would hit or miss pixel centers as it moves, so it covers one whole pixel kept by its share of that pixel.
    if (max(length(RightPixels), length(UpPixels)) < 0.5)
    {
        const float Coverage = 4.0 * abs(RightPixels.x * UpPixels.y - RightPixels.y * UpPixels.x);
        if (float(PCGHash(Slot) >> 8u) / 16777216.0 < Coverage)
        {
            Out.Position = ClipCenter + float4(C * ClipCenter.w / ScreenSize, 0.0, 0.0);
        }
        return Out;
    }

    Out.Position = ClipCenter + ClipRight * C.x + ClipUp * C.y;
    return Out;
}
