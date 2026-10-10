// Material samples take the gradients built from neighboring depth below, since quad derivatives break at depth edges and discards.
#define SHADING_EXPLICIT_GRADIENTS 1
#include "Includes/DecalCommon.slang"

// Projects the depth-reconstructed surface into the decal box and writes the channels the graph drives into the DBuffer.

struct PSInput
{
    nointerpolation uint DecalIndex : DECAL_INDEX;
    nointerpolation float DistanceFade : DECAL_FADE;
    float4 Position : SV_Position;
};

struct PSOutput
{
    float4 DBufferA : SV_Target0;   // rgb BaseColor,                a coverage
    float4 DBufferB : SV_Target1;   // rg  WorldNormal (OctEncode01), a coverage
    float4 DBufferC : SV_Target2;   // r Roughness g Metallic b AO,  a coverage
    float4 DBufferD : SV_Target3;   // rgb Emissive, added
};

float3 DecalSurfaceAt(float2 Pixel, float2 ScreenSize, out float RawDepth)
{
    float2 UV = Pixel / ScreenSize;
    RawDepth = SampleDecalSceneDepth(UV);
    return ReconstructWorldPos(UV, RawDepth, GetInverseCameraProjection(), GetInverseCameraView());
}

// The screen step toward whichever neighbor lies on the same surface, so a silhouette never becomes a gradient.
float3 SurfaceStep(float3 Center, float2 Pixel, float2 Offset, float2 ScreenSize)
{
    float BackDepth;
    float AheadDepth;
    float3 Back  = DecalSurfaceAt(Pixel - Offset, ScreenSize, BackDepth);
    float3 Ahead = DecalSurfaceAt(Pixel + Offset, ScreenSize, AheadDepth);
    float3 BackStep  = Center - Back;
    float3 AheadStep = Ahead - Center;
    if (BackDepth <= 0.0)
    {
        return AheadStep;
    }
    if (AheadDepth <= 0.0)
    {
        return BackStep;
    }
    return dot(BackStep, BackStep) < dot(AheadStep, AheadStep) ? BackStep : AheadStep;
}

// Fades toward the box's front and back faces, so a surface leaving the box does not end on a hard line.
float DecalAxisFade(FGPUDecal Decal, float LocalZ)
{
    float Depth = LocalZ + 0.5;
    float Upper = Decal.Fades.z > 0.0 ? saturate((1.0 - Depth) / Decal.Fades.z) : 1.0;
    float Lower = Decal.Fades.w > 0.0 ? saturate(Depth / Decal.Fades.w) : 1.0;
    return Upper * Lower;
}

[shader("fragment")]
PSOutput FragmentMain(PSInput Input)
{
    FGPUDecal Decal = Pass().Decals.Get(Input.DecalIndex);

    float2 ScreenSize = float2(Scene().ScreenSize.xy);
    float2 Pixel      = Input.Position.xy;

    float  RawDepth;
    float3 WorldPosition = DecalSurfaceAt(Pixel, ScreenSize, RawDepth);
    // No vertex graph in this domain, so there is no offset to exclude.
    float3 WorldPositionExcludingOffsets = WorldPosition;

    // The sky has no surface to receive a decal.
    if (RawDepth <= 0.0)
    {
        discard;
    }

    float3 Local = mul(Decal.WorldToDecal, float4(WorldPosition, 1.0)).xyz;
    if (any(abs(Local) > 0.5))
    {
        discard;
    }

    float3 WorldPosition_DDX = SurfaceStep(WorldPosition, Pixel, float2(1.0, 0.0), ScreenSize);
    float3 WorldPosition_DDY = SurfaceStep(WorldPosition, Pixel, float2(0.0, 1.0), ScreenSize);

    float3 Ngeo = normalize(cross(WorldPosition_DDX, WorldPosition_DDY));
    float3 ViewDir = GetCameraPosition() - WorldPosition;
    if (dot(Ngeo, ViewDir) < 0.0)
    {
        Ngeo = -Ngeo;
    }

    // Two-sided, so the box's Z may point either way, and only surfaces turned away from it fade.
    float3 DecalAxis = normalize(mul((float3x3)Decal.DecalToWorld, float3(0.0, 0.0, 1.0)));
    float  NdotAxis  = abs(dot(Ngeo, DecalAxis));
    float  AngleFade = saturate((NdotAxis - Decal.FadeAngleCos) / max(1.0 - Decal.FadeAngleCos, 1e-3));

    float Fade = AngleFade * Input.DistanceFade * DecalAxisFade(Decal, Local.z);
    if (Fade <= 0.0)
    {
        discard;
    }

    uint   MaterialIndex = Decal.MaterialIndex;
    float2 UV0           = (Local.xy + 0.5) * Decal.UVTransform.xy + Decal.UVTransform.zw;
    float3 WorldNormal   = Ngeo;
    float3 ViewPosition  = mul(GetCameraView(), float4(WorldPosition, 1.0)).xyz;
    float4 VertexColor   = float4(1.0, 1.0, 1.0, 1.0);
    uint   EntityID      = 0;
    // The decal's own X axis is its UV direction, so this is a real tangent frame, the same one the normal decode builds.
    float4 WorldTangent  = float4(normalize(mul((float3x3)Decal.DecalToWorld, float3(1.0, 0.0, 0.0))), 1.0);
    float2 UV0_DDX       = mul((float3x3)Decal.WorldToDecal, WorldPosition_DDX).xy * Decal.UVTransform.xy;
    float2 UV0_DDY       = mul((float3x3)Decal.WorldToDecal, WorldPosition_DDY).xy * Decal.UVTransform.xy;
    // A decal's UV is its own box projection, not mesh data, so set 1 aliases it.
    float2 UV1           = UV0;
    float2 UV1_DDX       = UV0_DDX;
    float2 UV1_DDY       = UV0_DDY;
    float3 WorldPositionExcludingOffsets_DDX = WorldPosition_DDX;
    float3 WorldPositionExcludingOffsets_DDY = WorldPosition_DDY;
    float4 VertexColor_DDX   = float4(0.0, 0.0, 0.0, 0.0);
    float4 VertexColor_DDY   = float4(0.0, 0.0, 0.0, 0.0);

    RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
    $MATERIAL_INPUTS

#ifndef DECAL_CHANNEL_MASK
#define DECAL_CHANNEL_MASK float4(1.0, 1.0, 1.0, 0.0)
#endif
    // Color, normal, surface and emission, each written only where the graph drives it.
    const float4 ChannelMask = DECAL_CHANNEL_MASK;

    float Coverage = saturate(Decal.Opacity * Fade * Material.Opacity);
    if (Coverage <= 0.0)
    {
        discard;
    }

    // Framed on the surface normal, so an unconnected normal pin (0, 0, 1) leaves it unchanged.
    float3 DecalX = normalize(mul((float3x3)Decal.DecalToWorld, float3(1.0, 0.0, 0.0)));
    float3 T = DecalX - Ngeo * dot(DecalX, Ngeo);
    float  TLen2 = dot(T, T);
    float3 WorldN = Ngeo;
    if (TLen2 > 1e-8)
    {
        T = T * rsqrt(TLen2);
        float3 B = cross(Ngeo, T);
        WorldN = normalize(T * Material.Normal.x + B * Material.Normal.y + Ngeo * Material.Normal.z);
    }

    PSOutput Output;
    Output.DBufferA = float4(Material.Diffuse * Decal.Modulate.rgb, Coverage * ChannelMask.x * Decal.Modulate.w);
    Output.DBufferB = float4(OctEncode01(WorldN), 0.0, Coverage * ChannelMask.y);
    Output.DBufferC = float4(Material.Roughness, Material.Metallic, Material.AmbientOcclusion, Coverage * ChannelMask.z);
    Output.DBufferD = float4(Material.Emissive * Decal.Modulate.rgb * (Decal.EmissionEnergy * Coverage * ChannelMask.w), 0.0);
    return Output;
}
