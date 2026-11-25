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
        FMeshletHeader* Header = MeshletHeaderAt(Input.HeaderSlot);

        // Taken before any lane can discard, since the threshold differentiates across the quad.
        const float Threshold = HashedAlphaThreshold(Input.WorldPos, length(Input.WorldPos - GetCameraPosition()), kImpostorCoverageCutoff);

        const float           InvLength = rsqrt(max(dot(Input.LocalDir, Input.LocalDir), 1.0e-20));
        const FImpostorViews  Views     = UnpackImpostorViews(Input.ViewFrames, Input.ViewWeights);
        const FImpostorTaps   Taps      = GatherImpostorTaps(Header, Views, Input.LocalOrigin, Input.LocalDir * InvLength, Input.Mip);
        if (Taps.Coverage < Threshold)
        {
            discard;
        }
        const FImpostorSample S = ResolveImpostorTaps(Header, Views, Taps, Input.Mip);

        // The local ray is the world one through the inverse model, so the hit lies the same share along the unnormalized world ray.
        const float3 WorldDir = IsCameraOrthographic() ? GetCameraForward() : Input.WorldPos - GetCameraPosition();
        const float4 Clip     = WorldToClip(Input.WorldPos + WorldDir * (S.RayT * InvLength));
        Out.Depth = saturate(Clip.z / Clip.w);
    }
    return Out;
}
