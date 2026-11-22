#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/HashedAlpha.slang"
#include "Includes/Impostor.slang"
#include "Includes/ProxyVisBuffer.slang"

// Stamps the proxy's VisBuffer word, which ProxyVisBufferVert.slang built with the proxy bit set, where its impostor covers the pixel and at the depth it baked.

struct FProxyPixelOutput
{
    uint  VisID : SV_Target0;
    float Depth : SV_Depth;
};

[shader("fragment")]
FProxyPixelOutput FragmentMain(FProxyVertexOutput Input)
{
    FProxyPixelOutput Out;
    Out.VisID = Input.VisID;
    Out.Depth = Input.Position.z;

    if (Input.bAlphaTest != 0u)
    {
        const FVisProxy P      = LoadProxyEntry(Input.Entry);
        FMeshletHeader* Header = MeshletHeaderAt(P.Static.MeshletHeaderSlot);

        float3 GroundWiden;
        ProxyWidenedCenter(P.Model, P.Proxy, GroundWiden);
        const float4x4 Model = ProxyWidenedModel(P.Model, GroundWiden);

        // Taken before any lane can discard, since the threshold differentiates across the quad.
        const float Threshold = HashedAlphaThreshold(Input.WorldPos, length(Input.WorldPos - GetCameraPosition()), kImpostorCoverageCutoff);

        const FImpostorRay    Ray    = MakeImpostorRay(Header, Model, Input.WorldPos);
        const FImpostorViews  Views  = SelectImpostorViews(Ray.ToEye, Header.ImpostorFrames);
        const float3          Center = mul(Model, float4(Header.ImpostorCenter, 1.0)).xyz;
        const FImpostorSample S      = SampleImpostor(Header, Views, Ray.Origin, Ray.Dir, ImpostorMipLevel(Header, Center, Header.ImpostorRadius * MatrixMaxScale(P.Model)));
        if (S.Coverage < Threshold)
        {
            discard;
        }

        const float4 Clip = WorldToClip(mul(Model, float4(Ray.Origin + Ray.Dir * S.RayT, 1.0)).xyz);
        Out.Depth = saturate(Clip.z / Clip.w);
    }
    return Out;
}
