#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/Impostor.slang"
#include "Includes/ProxyVisBuffer.slang"

// One camera-facing quad per retained slot or impostor point the cull listed as too small for its meshlets, stamped into the VisBuffer with the proxy bit.

// The rect in the camera plane through Center that holds the perspective image of the mesh's box, grown by Margin and kept inside the sphere's square.
bool ImpostorSilhouetteRect(FMeshletHeader* Header, float4x4 Model, float3 Center, float HalfSize, float Margin, out float2 Min, out float2 Max)
{
    const float3 Eye        = GetCameraPosition();
    const float3 Forward    = GetCameraForward();
    const bool   bOrtho     = IsCameraOrthographic();
    const float  PlaneDepth = dot(Center - Eye, Forward);
    const float3 BoxMin     = Header.LocalBoundsMin;
    const float3 BoxMax     = Header.LocalBoundsMax;
    if (any(BoxMax <= BoxMin))
    {
        return false;
    }

    Min = float2(1.0e30, 1.0e30);
    Max = float2(-1.0e30, -1.0e30);
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const float3 Local = float3((i & 1u) != 0u ? BoxMax.x : BoxMin.x, (i & 2u) != 0u ? BoxMax.y : BoxMin.y, (i & 4u) != 0u ? BoxMax.z : BoxMin.z);
        const float3 World = mul(Model, float4(Local, 1.0)).xyz;
        const float  Depth = dot(World - Eye, Forward);
        if (!bOrtho && Depth <= 1.0e-3)
        {
            return false;
        }
        const float3 OnPlane = bOrtho ? World - Forward * (Depth - PlaneDepth) : Eye + (World - Eye) * (PlaneDepth / Depth);
        const float2 P       = float2(dot(OnPlane - Center, GetCameraRight()), dot(OnPlane - Center, GetCameraUp()));
        Min = min(Min, P);
        Max = max(Max, P);
    }
    Min = max(Min - Margin, -HalfSize);
    Max = min(Max + Margin, HalfSize);
    return all(Min < Max);
}

[shader("vertex")]
FProxyVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FProxyVertexOutput Out;
    Out.VisID      = VISBUFFER_PROXY_BIT | (InstanceIndex + 1u);
    Out.HeaderSlot  = 0u;
    Out.bAlphaTest  = 0u;
    Out.ViewFrames  = uint3(0u, 0u, 0u);
    Out.ViewWeights = float3(0.0, 0.0, 0.0);
    Out.Mip         = 0.0;
    Out.WorldPos    = float3(0.0, 0.0, 0.0);
    Out.LocalOrigin = float3(0.0, 0.0, 0.0);
    Out.LocalDir    = float3(0.0, 0.0, 1.0);
    Out.Position    = float4(0.0, 0.0, 0.0, 0.0);

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
            const FImpostorViews Views = SelectImpostorViews(Header, Ray.ToEye);
            Coverage *= GatherImpostorTaps(Header, Views, Ray.Origin, Ray.Dir, log2(float(Header.ImpostorFrameSize))).Coverage;
        }
        if (float(PCGHash(Entry) >> 8u) / 16777216.0 < Coverage)
        {
            Out.Position = ClipCenter + float4(C * ClipCenter.w / ScreenSize, 0.0, 0.0);
        }
        return Out;
    }

    Out.HeaderSlot = P.Static.MeshletHeaderSlot;
    Out.bAlphaTest = bImpostor ? 1u : 0u;
    Out.WorldPos   = Center + WorldRight * C.x + WorldUp * C.y;
    Out.Position   = ClipCenter + ClipRight * C.x + ClipUp * C.y;

    if (bImpostor)
    {
        const float WorldRadius = Header.ImpostorRadius * MatrixMaxScale(P.Model);
        Out.Mip = ImpostorMipLevel(Header, Center, WorldRadius);

        // A plant fills a tenth of its sphere's square, and every pixel of the rest would still run the view taps, so the quad hugs the box.
        const float TexelMargin = 2.0 * WorldRadius * exp2(Out.Mip) / float(Header.ImpostorFrameSize);
        float2 RectMin, RectMax;
        if (ImpostorSilhouetteRect(Header, Model, Center, HalfSize * max(GroundWiden.x, max(GroundWiden.y, GroundWiden.z)), TexelMargin, RectMin, RectMax))
        {
            const float2 Corner = lerp(RectMin, RectMax, C * 0.5 + 0.5);
            Out.WorldPos = Center + GetCameraRight() * Corner.x + GetCameraUp() * Corner.y;
            Out.Position = WorldToClip(Out.WorldPos);
        }

        // Constant across the quad, so it runs for four corners here rather than for every pixel it covers.
        const float4x4 WorldToLocal = MakeInstanceWorldToLocal(Model);
        const bool     bOrtho       = IsCameraOrthographic();
        Out.LocalOrigin = mul(WorldToLocal, float4(Out.WorldPos, 1.0)).xyz;
        Out.LocalDir    = mul((float3x3)WorldToLocal, bOrtho ? GetCameraForward() : Out.WorldPos - GetCameraPosition());

        const float3 ToEye = bOrtho ? -normalize(Out.LocalDir) : normalize(mul(WorldToLocal, float4(GetCameraPosition(), 1.0)).xyz - Header.ImpostorCenter);
        PackImpostorViews(SelectImpostorViews(Header, ToEye), Out.ViewFrames, Out.ViewWeights);
    }
    return Out;
}
