#include "Includes/SurfaceShading.slang"

// Masked VisBuffer pixel shader. Runs the material's pixel graph far enough to evaluate Material.Opacity and
// `discard`s cut-out texels BEFORE they write VisID/depth, so masked geometry no longer stamps full-coverage
// depth over whatever is behind its holes. The deferred material pass keeps its own ShadeSurface masked clip
// for silhouette edges; this one fixes coverage/occlusion.
//
// Two entry points share one opacity evaluation:
//   - default        : VS-emulation path. Input = FVisVertexOut (interpolants + flat VisID).
//   - VISBUFFER_PRIMID: mesh-shader path. Input = FBaseVertexOutput (per-vertex) + VisID via SV_PrimitiveID.
// The material-graph token appears exactly once (inside MaskedVisKeep): the compiler substitutes the FIRST
// occurrence, so it must never appear in a comment above it -- that would inject the codegen at file scope.

// Runs the pixel graph and returns true when the texel should be KEPT (opacity >= the material's cutoff).
// `discard` is legal from a helper called by a pixel shader. Reuses the exact local contract of BasePixelPass.
bool MaskedVisKeep(FBaseVertexOutput Input)
{
    uint EntityID           = Input.EntityID;
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = normalize(Input.NormalWS.xyz);
    float3 ViewPosition     = Input.FragPos.xyz;
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0              = Input.UV;
    float4 VertexColor      = Input.FragColor;

    $MATERIAL_INPUTS

    return Material.Opacity >= Materials()[MaterialIndex].OpacityClipValue;
}

#ifdef VISBUFFER_PRIMID

[shader("fragment")]
uint FragmentMain(FBaseVertexOutput Input, uint VisID : SV_PrimitiveID) : SV_Target0
{
    if (!MaskedVisKeep(Input))
    {
        discard;
    }
    return VisID;
}

#else

[shader("fragment")]
uint FragmentMain(FVisVertexOut In) : SV_Target0
{
    if (!MaskedVisKeep(In.Base))
    {
        discard;
    }
    return In.VisID;
}

#endif
