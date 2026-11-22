#include "Includes/ImpostorShadow.slang"

// The impostor as the sun sees it, kept where its views cover the texel and written at the depth the bake stored there.

[shader("fragment")]
void FragmentMain(FImpostorShadowVertexOutput In)
{
    const FVisProxy P = LoadProxyEntry(In.Entry);
    FMeshletHeader* Header = MeshletHeaderAt(P.Static.MeshletHeaderSlot);
    if (!P.bValid || !Header.bHasImpostor)
    {
        discard;
    }

    const float4x4 Model        = ImpostorShadowModel(P);
    const float4x4 WorldToLocal = MakeInstanceWorldToLocal(Model);
    const float3   SunRay       = -normalize(GetSunDirection());
    const float3   Origin       = mul(WorldToLocal, float4(In.WorldPos, 1.0)).xyz;
    const float3   Dir          = normalize(mul((float3x3)WorldToLocal, SunRay));

    // The mip whose texel is about one shadow texel across the sphere.
    const float TexelWorld = max(VirtualShadowLevel(In.Level).TexelWorld, 1.0e-4);
    const float Texels     = Header.ImpostorRadius * MatrixMaxScale(Model) / TexelWorld;
    const float Mip        = max(log2(float(Header.ImpostorFrameSize) / max(Texels, 1.0e-3)), 0.0);

    const FImpostorViews  Views = SelectImpostorViews(-Dir, Header.ImpostorFrames);
    const FImpostorSample S     = SampleImpostor(Header, Views, Origin, Dir, Mip);
    if (S.Coverage < kImpostorCoverageCutoff)
    {
        discard;
    }

    // The baked depth is 8 bits across the sphere and block compressed, so an unbiased caster shadows its own receiving pixels by chance as the views blend.
    const float RayT = S.RayT + kImpostorShadowDepthBias * Header.ImpostorRadius;
    const float4 Clip = mul(ImpostorShadowViewProjection(In.Level), float4(mul(Model, float4(Origin + Dir * RayT, 1.0)).xyz, 1.0));
    WriteVirtualShadowLevelDepth(In.Level, true, float4(In.Position.xy, Clip.z / Clip.w, 1.0));
}
