#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/Impostor.slang"
#include "Includes/ProxyVisBuffer.slang"

// One camera-facing quad per retained slot or impostor point the cull listed as too small for its meshlets, stamped into the VisBuffer with the proxy bit.

[shader("vertex")]
FProxyVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FProxyVertexOutput Out;
    Out.VisID      = VISBUFFER_PROXY_BIT | (InstanceIndex + 1u);
    Out.Entry      = 0u;
    Out.bAlphaTest = 0u;
    Out.WorldPos   = float3(0.0, 0.0, 0.0);
    Out.Position   = float4(0.0, 0.0, 0.0, 0.0);

    if (!ProxyList().Contains(InstanceIndex))
    {
        return Out;
    }
    const uint      Entry = ProxyList().Get(InstanceIndex);
    const FVisProxy P     = LoadProxyEntry(Entry);
    if (!P.bValid)
    {
        return Out;
    }

    FMeshletHeader* Header    = MeshletHeaderAt(P.Static.MeshletHeaderSlot);
    const bool      bImpostor = Header.bHasImpostor;

    float3 GroundWiden;
    float3 Center   = ProxyWidenedCenter(P.Model, P.Proxy, GroundWiden);
    float  HalfSize = P.Proxy.SplatHalfSize * MatrixMaxScale(P.Model);
    const float4x4 Model = ProxyWidenedModel(P.Model, GroundWiden);
    if (bImpostor)
    {
        // The quad frames the sphere the bake framed.
        Center   = mul(Model, float4(Header.ImpostorCenter, 1.0)).xyz;
        HalfSize = Header.ImpostorRadius * MatrixMaxScale(P.Model) * kImpostorQuadGrowth;
    }

    // A strip's two triangles, so the quad's shared corners are emitted once.
    static const float2 Corners[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    const float2 C = Corners[min(VertexIndex, 3u)];

    // Clip is linear in position, so the corner offsets project once as directions and add to the center's image.
    const float3   WorldRight = GetCameraRight() * HalfSize * GroundWiden;
    const float3   WorldUp    = GetCameraUp() * HalfSize * GroundWiden;
    const float4x4 Projection = GetCameraProjection();
    const float4   ClipCenter = WorldToClip(Center);
    const float4   ClipRight  = mul(Projection, float4(NormalWorldToView(WorldRight), 0.0));
    const float4   ClipUp     = mul(Projection, float4(NormalWorldToView(WorldUp), 0.0));

    const float2 ScreenSize   = float2(GetScreenSize());
    const float2 ClipToPixels = ScreenSize * 0.5 / ClipCenter.w;
    const float2 RightPixels  = ClipRight.xy * ClipToPixels;
    const float2 UpPixels     = ClipUp.xy * ClipToPixels;

    // A splat narrower than a pixel would hit or miss pixel centers as it moves, so it covers one whole pixel kept by its share of that pixel.
    if (max(length(RightPixels), length(UpPixels)) < 0.5)
    {
        float Coverage = 4.0 * abs(RightPixels.x * UpPixels.y - RightPixels.y * UpPixels.x);
        if (bImpostor)
        {
            // An impostor's coarsest mip holds a few texels per view, which the view's center averages.
            const FImpostorRay   Ray   = MakeImpostorRay(Header, Model, Center);
            const FImpostorViews Views = SelectImpostorViews(Ray.ToEye, Header.ImpostorFrames);
            Coverage *= SampleImpostor(Header, Views, Ray.Origin, Ray.Dir, log2(float(Header.ImpostorFrameSize))).Coverage;
        }
        if (float(PCGHash(Entry) >> 8u) / 16777216.0 < Coverage)
        {
            Out.Position = ClipCenter + float4(C * ClipCenter.w / ScreenSize, 0.0, 0.0);
        }
        return Out;
    }

    Out.Entry      = Entry;
    Out.bAlphaTest = bImpostor ? 1u : 0u;
    Out.WorldPos   = Center + WorldRight * C.x + WorldUp * C.y;
    Out.Position   = ClipCenter + ClipRight * C.x + ClipUp * C.y;
    return Out;
}
