#include "Includes/DecalCommon.slang"

// Decal material template (EMaterialType::Decal). Rasterized as a box; for each covered pixel it
// reconstructs the shaded surface position from scene depth, projects into decal-local space, runs the
// material graph at the projected UV, and writes BaseColor / WorldNormal / Roughness-Metallic-AO into
// the DBuffer. The base pass composites these before lighting (see Includes/DBuffer.slang).

struct PSInput
{
    nointerpolation uint DecalIndex : DECAL_INDEX;
    float4 Position : SV_Position;
};

struct PSOutput
{
    float4 DBufferA : SV_Target0;   // rgb BaseColor,                a coverage
    float4 DBufferB : SV_Target1;   // rg  WorldNormal (OctEncode01), a coverage
    float4 DBufferC : SV_Target2;   // r Roughness g Metallic b AO,  a coverage
};

[shader("fragment")]
PSOutput FragmentMain(PSInput Input)
{
    FGPUDecal Decal = Pass().Decals[Input.DecalIndex];

    uint2  ScreenSize = Scene().ScreenSize.xy;
    float2 ScreenUV   = Input.Position.xy / float2(ScreenSize);

    // Reconstruct the shaded surface world position from opaque depth at this pixel.
    float RawDepth = SampleDecalSceneDepth(ScreenUV);
    float3 WorldPosition = ReconstructWorldPos(ScreenUV, RawDepth, GetInverseCameraProjection(), GetInverseCameraView());

    // Reject the sky / far plane (no geometry to receive the decal).
    if (RawDepth <= 0.0)
    {
        discard;
    }

    // Project into the decal's local cube; outside the unit box -> no decal here.
    float3 Local = mul(Decal.WorldToDecal, float4(WorldPosition, 1.0)).xyz;
    if (any(abs(Local) > 0.5))
    {
        discard;
    }

    // Geometric surface normal from depth derivatives, oriented toward the camera.
    float3 Ngeo = normalize(cross(ddx(WorldPosition), ddy(WorldPosition)));
    float3 ViewDir = GetCameraPosition() - WorldPosition;
    if (dot(Ngeo, ViewDir) < 0.0)
    {
        Ngeo = -Ngeo;
    }

    // Angle fade: the decal applies to surfaces parallel to its XY plane, i.e. whose normal aligns with
    // the box's Z axis -- two-sided (abs), so it works no matter which way the box's Z axis is pointed.
    // It only fades surfaces angled away (grazing/perpendicular) to prevent projection smear on walls.
    float3 DecalAxis = normalize(mul((float3x3)Decal.DecalToWorld, float3(0.0, 0.0, 1.0)));
    float  NdotAxis  = abs(dot(Ngeo, DecalAxis));
    float  AngleFade = saturate((NdotAxis - Decal.FadeAngleCos) / max(1.0 - Decal.FadeAngleCos, 1e-3));
    if (AngleFade <= 0.0)
    {
        discard;
    }

    // Material graph aliases. UV0 is the projected decal UV (local XY -> [0,1]).
    uint   MaterialIndex = Decal.MaterialIndex;
    float2 UV0           = Local.xy + 0.5;
    float3 WorldNormal   = Ngeo;
    float3 ViewPosition  = mul(GetCameraView(), float4(WorldPosition, 1.0)).xyz;
    float4 VertexColor   = float4(1.0, 1.0, 1.0, 1.0);
    uint   EntityID      = 0;
    // Graph local contract. The decal's own X axis IS its UV direction (UV0 is Local.xy), so this is a
    // genuine tangent frame, not a placeholder -- it matches the one the normal decode below builds.
    float4 WorldTangent  = float4(normalize(mul((float3x3)Decal.DecalToWorld, float3(1.0, 0.0, 0.0))), 1.0);
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);

    $MATERIAL_INPUTS

    // Coverage drives every DBuffer target's alpha (blend writes transmittance into a).
    float Coverage = saturate(Decal.Opacity * AngleFade * Material.Opacity);
    if (Coverage <= 0.0)
    {
        discard;
    }

    // Tangent-space decal normal -> world, framed on the actual surface normal so an unconnected
    // normal pin (default (0,0,1)) leaves the surface normal unchanged.
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
    Output.DBufferA = float4(Material.Diffuse, Coverage);
    Output.DBufferB = float4(OctEncode01(WorldN), 0.0, Coverage);
    Output.DBufferC = float4(Material.Roughness, Material.Metallic, Material.AmbientOcclusion, Coverage);
    return Output;
}
