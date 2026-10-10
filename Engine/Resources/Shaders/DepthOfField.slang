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
    float _Pad;
};
FDepthOfFieldPC* Pass() { return GetPassArgs<FDepthOfFieldPC>(); }

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

static const float GOLDEN_ANGLE = 2.39996323;
static const float SKY_DISTANCE = 100000.0;

float SceneDistance(float2 UV)
{
    const float Raw = Pass().SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
    if (Raw <= 0.0)
    {
        return SKY_DISTANCE;
    }
    return max(ReconstructViewPos(UV, Raw, GetInverseCameraProjection()).z, 0.01);
}

// Blur disc radius in pixels for a surface Distance meters away, from the thin lens equation.
float BlurRadius(float Distance, float Scale)
{
    return min(Scale * abs(Distance - Pass().FocusDistance) / Distance, Pass().MaxRadius);
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

    const float CenterDistance = SceneDistance(Input.UV);
    const float CenterRadius = BlurRadius(CenterDistance, Scale);

    float3 Color = Pass().SceneColor.SampleLevel(Samplers::LinearClamp, Input.UV, 0.0).rgb;
    float Total = 1.0;
    float Radius = Pass().RadiusStep;
    for (float Angle = 0.0; Radius < Pass().MaxRadius; Angle += GOLDEN_ANGLE)
    {
        const float2 SampleUV = Input.UV + float2(cos(Angle), sin(Angle)) * Texel * Radius;
        const float3 SampleColor = Pass().SceneColor.SampleLevel(Samplers::LinearClamp, SampleUV, 0.0).rgb;
        const float SampleDistance = SceneDistance(SampleUV);
        float SampleRadius = BlurRadius(SampleDistance, Scale);
        // Something behind a sharper subject may only bleed as far as the subject itself is blurred.
        if (SampleDistance > CenterDistance)
        {
            SampleRadius = clamp(SampleRadius, 0.0, CenterRadius * 2.0);
        }
        const float Coverage = smoothstep(Radius - 0.5, Radius + 0.5, SampleRadius);
        Color += lerp(Color / Total, SampleColor, Coverage);
        Total += 1.0;
        Radius += Pass().RadiusStep / Radius;
    }

    return float4(Color / Total, 1.0);
}
