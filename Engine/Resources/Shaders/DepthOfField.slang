#include "Includes/SceneGlobals.slang"

// Physical depth of field gathered from a copy of the HDR scene, where each sample counts if its own blur disc covers this pixel.

struct FDepthOfFieldPC
{
    FTexture2D SceneColor;
    FTexture2D SceneDepth;
    float      FStop;
    float      FocusDistance;       // meters

    float SensorHeight;        // millimeters
    float MaxRadius;           // pixels
    float RadiusStep;          // pixels added per ring, scaled so the sample count stays bounded
    float BandRadius;          // pixels of extra blur far outside the sharp screen band, zero when the band is off

    float BandCenter;          // screen UV height
    float BandHalfHeight;
    float BandFalloff;
    float BladeSector;         // radians spanned by one diaphragm blade

    float LensRadius;          // lens circle rounding the blade corners, in units of the polygon's apothem
    float BokehScale;          // scales the rounded polygon to the area of the unit circle
    float2 _Pad;

    float4 FocalPlane;         // view space normal and offset, the plane z = FocusDistance when untilted
};
FDepthOfFieldPC* Pass() { return GetPassArgs<FDepthOfFieldPC>(); }

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

static const float GOLDEN_ANGLE = 2.39996323;
static const float SKY_DISTANCE = 100000.0;

float3 SceneViewPos(float2 UV)
{
    const float Raw = Pass().SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
    if (Raw <= 0.0)
    {
        const float3 NearPos = ReconstructViewPos(UV, 1.0, GetInverseCameraProjection());
        return float3(NearPos.xy * (SKY_DISTANCE / max(NearPos.z, 0.01)), SKY_DISTANCE);
    }
    const float3 ViewPos = ReconstructViewPos(UV, Raw, GetInverseCameraProjection());
    return float3(ViewPos.xy, max(ViewPos.z, 0.01));
}

// Signed circle of confusion in pixels against the focal plane, positive in front of focus, grown outside the sharp screen band.
float SignedCoc(float3 ViewPos, float ScreenY, float Scale)
{
    const float Coc = Scale * (Pass().FocalPlane.w - dot(Pass().FocalPlane.xyz, ViewPos)) / ViewPos.z;
    const float BandOffset = abs(ScreenY - Pass().BandCenter) - Pass().BandHalfHeight;
    const float BandRadius = Pass().BandRadius * saturate(BandOffset / Pass().BandFalloff);
    return clamp(Coc + (Coc < 0.0 ? -BandRadius : BandRadius), -Pass().MaxRadius, Pass().MaxRadius);
}

// Distance from the bokeh center to its edge along Angle, relative to a circle of the same area.
float ApertureEdge(float Angle)
{
    const float Phi = fmod(Angle, Pass().BladeSector) - Pass().BladeSector * 0.5;
    return min(1.0 / cos(Phi), Pass().LensRadius) * Pass().BokehScale;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    const float2 ScreenSize = float2(GetScreenSize());
    const float2 Texel = 1.0 / ScreenSize;

    // Focal length from the vertical field of view and film back, then the circle of confusion scale in pixels.
    const float FocalMM = Pass().SensorHeight * 0.5 * GetCameraProjection()[1][1];
    const float FocusMM = max(Pass().FocusDistance * 1000.0, FocalMM + 1.0);
    const float Scale = (FocalMM * FocalMM) / (Pass().FStop * (FocusMM - FocalMM)) / Pass().SensorHeight * ScreenSize.y * 0.5;

    const float3 CenterPos = SceneViewPos(Input.UV);
    const float CenterRadius = abs(SignedCoc(CenterPos, Input.UV.y, Scale));

    float3 Color = Pass().SceneColor.SampleLevel(Samplers::LinearClamp, Input.UV, 0.0).rgb;
    float Total = 1.0;
    float Radius = Pass().RadiusStep;
    for (float Angle = 0.0; Radius < Pass().MaxRadius; Angle += GOLDEN_ANGLE)
    {
        const float2 SampleUV = Input.UV + float2(cos(Angle), sin(Angle)) * Texel * Radius;
        const float3 SampleColor = Pass().SceneColor.SampleLevel(Samplers::LinearClamp, SampleUV, 0.0).rgb;
        const float3 SamplePos = SceneViewPos(SampleUV);
        float SampleRadius = abs(SignedCoc(SamplePos, SampleUV.y, Scale));
        // Something behind a sharper subject may only bleed as far as the subject itself is blurred.
        if (SamplePos.z > CenterPos.z)
        {
            SampleRadius = clamp(SampleRadius, 0.0, CenterRadius * 2.0);
        }
        const float Coverage = smoothstep(Radius - 0.5, Radius + 0.5, SampleRadius * ApertureEdge(Angle));
        Color += lerp(Color / Total, SampleColor, Coverage);
        Total += 1.0;
        Radius += Pass().RadiusStep / Radius;
    }

    return float4(Color / Total, 1.0);
}
