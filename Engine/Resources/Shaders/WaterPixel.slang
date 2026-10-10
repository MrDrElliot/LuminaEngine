#include "Includes/Water.slang"
#include "Includes/Noise.slang"
#include "Includes/IBLRuntime.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/Fog.slang"
#include "Includes/AerialPerspective.slang"

struct PSInput
{
    float4 Position : SV_Position;
    float3 WorldPos : WATER_WORLDPOS;
    float2 BaseXZ   : WATER_BASEXZ;
    nointerpolation uint WaterIndex : WATER_INDEX;
};

// View space is +Z forward; visible geometry has positive view Z.
float SceneDistFromRaw(float2 UV, float RawDepth)
{
    return ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection()).z;
}

float SampleSceneDepthRaw(float2 UV)
{
    return Pass().SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
}

// Compared as linear distance, since a fixed raw reverse-Z tolerance grows with distance squared and lets far water swallow low land.
bool IsSceneInFrontOfWater(float2 UV, float SceneRaw, float WaterDist)
{
    return SceneRaw > 0.0 && SceneDistFromRaw(UV, SceneRaw) < WaterDist - (0.01 + WaterDist * 1e-4);
}

float3 SampleSceneColor(float2 UV)
{
    return Pass().SceneColor.SampleLevel(Samplers::LinearClamp, UV, 0.0).rgb;
}

float WaterFbm(float2 P)
{
    // Each octave turned against the last, since axis-aligned value noise shows its lattice as a grid of blobs.
    const float2x2 Turn = float2x2(0.8, -0.6, 0.6, 0.8);
    float Sum = 0.0;
    float Amp = 0.5;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        Sum += Amp * ValueNoise2D(P);
        P    = mul(Turn, P) * 2.03 + 17.0;
        Amp *= 0.5;
    }
    return Sum;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FGPUWater W = Pass().Waters.Get(Input.WaterIndex);

    uint2  ScreenSize = Scene().ScreenSize.xy;
    float2 ScreenUV   = Input.Position.xy / float2(ScreenSize);

    float  Footprint = max(length(ddx(Input.BaseXZ)), length(ddy(Input.BaseXZ)));
    FWaterSurface Surface = EvaluateWaterSurface(Input.BaseXZ, W, GetTime(), Footprint);

    // Opaque geometry in front is rejected by the depth test, so the copy is read only for what lies behind.
    float SceneRawDepth = SampleSceneDepthRaw(ScreenUV);
    float WaterDist     = SceneDistFromRaw(ScreenUV, Input.Position.z);

    float3 N = Surface.Normal;
    if (W.DetailNormal.IsValid())
    {
        float  TileSize = max(W.DetailParams.x, 0.01);
        float  Speed    = W.DetailParams.y;
        float2 Wind     = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));

        // Two scales at an angle to each other so the tiling never lines up, faded out once a tile shrinks below a few pixels.
        float2 UV0 = Input.BaseXZ / TileSize + Wind * (GetTime() * Speed);
        float2 UV1 = float2(Input.BaseXZ.x * 0.8 - Input.BaseXZ.y * 0.6, Input.BaseXZ.x * 0.6 + Input.BaseXZ.y * 0.8) / (TileSize * 0.37) - Wind * (GetTime() * Speed * 1.3);
        float2 n0  = W.DetailNormal.Sample(Samplers::LinearWrap, UV0).rg * 2.0 - 1.0;
        float2 n1  = W.DetailNormal.Sample(Samplers::LinearWrap, UV1).rg * 2.0 - 1.0;
        float  DetailFade = smoothstep(4.0, 16.0, TileSize * 0.37 / max(Footprint, 1e-4));
        float2 dn  = (n0 + n1) * W.WaveParams.w * DetailFade;

        float3 T = normalize(float3(1.0, 0.0, 0.0) - N * N.x);
        float3 B = cross(N, T);
        N = normalize(N + (T * dn.x + B * dn.y));
    }

    // Sub-pixel wave slope folded into roughness, which keeps the sun's glitter path coherent at distance.
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float  SlopeVariance   = 0.5 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float  KernelRoughness = min(2.0 * SlopeVariance, 0.25);

    float  Roughness  = W.RefractReflect.z;
    float  BaseAlpha  = max(Roughness * Roughness, 1e-4) + KernelRoughness;
    float  SpecAlpha  = saturate(sqrt(BaseAlpha * BaseAlpha + 2.0 * Surface.FilteredVariance));
    float  EffRough   = sqrt(SpecAlpha);

    float3 CameraPos = GetCameraPosition();
    float3 V     = normalize(CameraPos - Input.WorldPos);
    float  NdotV = saturate(dot(N, V));

    bool   HasScene      = (SceneRawDepth > 0.0);
    float3 SceneViewPos  = ReconstructViewPos(ScreenUV, HasScene ? SceneRawDepth : 1e-5, GetInverseCameraProjection());
    float3 SceneWorldPos = mul(GetInverseCameraView(), float4(SceneViewPos, 1.0)).xyz;
    float  WaterViewDist = WorldToView(Input.WorldPos).z;
    float  SceneViewDist = HasScene ? SceneViewPos.z : GetFarPlane();
    float  ColumnDepth   = max(SceneViewDist - WaterViewDist, 0.0);
    float  DepthFade     = saturate(ColumnDepth / max(W.FoamAbsorb.z, 0.01));

    // Submerged only if the scene behind is inside the water's XZ box and below its surface.
    float3 SceneLocal   = mul(W.WorldToWater, float4(SceneWorldPos, 1.0)).xyz;
    float  InsideXZ     = saturate((0.5 - max(abs(SceneLocal.x), abs(SceneLocal.z))) * 16.0);
    float  BelowSurface = saturate((Input.WorldPos.y - SceneWorldPos.y) * 4.0);
    float  SubmergeMask = HasScene ? (InsideXZ * BelowSurface) : 0.0;

    float2 RefractOffset = N.xz * W.RefractReflect.x * (0.25 + 0.75 * DepthFade);
    float2 RefractUV     = ScreenUV + RefractOffset;
    float  RefrRaw       = SampleSceneDepthRaw(RefractUV);
    if (IsSceneInFrontOfWater(RefractUV, RefrRaw, WaterDist) || any(saturate(RefractUV) != RefractUV))
    {
        RefractUV = ScreenUV;
    }
    float3 BehindColor = SampleSceneColor(RefractUV);


    // The water and foam colors are what the surface scatters back, so they take the light a flat diffuse surface would.
    float3 BodyLight = (HasIBL() ? SampleIrradiance(float3(0.0, 1.0, 0.0)) : GetAmbientLightColor()) * GetAmbientLightIntensity();
    float  SunShadow = 1.0;
    if (LightData().bHasSun != 0)
    {
        FLight Sun = GetLightAt(0);
        float3 L   = normalize(GetSunDirection());
        SunShadow  = ComputeDirectionalShadow(Sun, Input.WorldPos, N, WorldToView(Input.WorldPos), Input.Position.xy);
        BodyLight += GetLightColor(Sun).rgb * Sun.Intensity * saturate(L.y) * SunShadow * INV_PI;
    }

    // WaterColor tints what is seen through the water, while the lit colors are light the volume itself sends back.
    float3 WaterColor = lerp(W.ShallowColor.rgb, W.DeepColor.rgb, DepthFade);
    float  Murk       = max(W.FoamAbsorb.w, 0.0);
    float  BedVis     = exp(-(2.0 + ColumnDepth) * Murk * (3.0 / max(W.FoamAbsorb.z, 0.1)));
    float3 Refracted  = lerp(W.DeepColor.rgb * BodyLight, BehindColor * WaterColor, BedVis);
    Refracted = lerp(WaterColor * BodyLight, Refracted, SubmergeMask);

    // Kept above the horizon, since a steep facet would otherwise reflect the dark lower sky as black streaks.
    float  WaterPlaneY = mul(W.WaterToWorld, float4(0.0, 0.0, 0.0, 1.0)).y;
    float3 R         = reflect(-V, N);
    R = normalize(float3(R.x, max(R.y, 0.03), R.z));
    float3 SkyRefl   = SamplePrefilter(R, EffRough);
    float3 Reflection = SkyRefl;
    {
        float  StepCount = max(W.SSRSpecOpacity.y, 8.0);
        float  MaxDist   = W.SSRSpecOpacity.x;
        float3 StartV    = mul(GetCameraView(), float4(Input.WorldPos, 1.0)).xyz;
        float3 DirV      = normalize(mul((float3x3)GetCameraView(), R));
        if (DirV.z > 0.001)   // view space is +Z forward
        {
            float  StepLen = MaxDist / StepCount;
            float3 Pv      = StartV;
            bool   Hit     = false;
            float2 HitUV   = ScreenUV;

            [loop]
            for (int s = 0; s < (int)StepCount; ++s)
            {
                Pv += DirV * StepLen;
                float4 Clip = mul(GetCameraProjection(), float4(Pv, 1.0));
                if (Clip.w <= 0.0)
                {
                    break;
                }
                float2 Suv = (Clip.xy / Clip.w) * float2(0.5, 0.5) + 0.5;
                if (any(saturate(Suv) != Suv))
                {
                    break;
                }
                float SampleRaw = SampleSceneDepthRaw(Suv);
                if (SampleRaw <= 0.0)
                {
                    continue;
                }
                float SampleDist = SceneDistFromRaw(Suv, SampleRaw);
                float RayDist    = Pv.z;
                if (RayDist > SampleDist && (RayDist - SampleDist) < StepLen * 3.0)
                {
                    // Geometry under the surface is hidden from a reflection off it, so a hit down there is a miss.
                    float3 HitWorld = mul(GetInverseCameraView(), float4(ReconstructViewPos(Suv, SampleRaw, GetInverseCameraProjection()), 1.0)).xyz;
                    if (HitWorld.y < WaterPlaneY - 0.25)
                    {
                        break;
                    }
                    Hit   = true;
                    HitUV = Suv;
                    break;
                }
            }

            if (Hit)
            {
                float2 E        = abs(HitUV - 0.5) * 2.0;
                float  EdgeFade = smoothstep(0.0, 0.3, saturate(1.0 - max(E.x, E.y)));
                Reflection = lerp(SkyRefl, SampleSceneColor(HitUV), EdgeFade);
            }
        }
    }

    const float F0 = 0.02;
    float Fresnel = saturate(F0 + (1.0 - F0) * pow(1.0 - NdotV, W.RefractReflect.w)) * W.RefractReflect.y;

    float3 Color = lerp(Refracted, Reflection, Fresnel);

    if (LightData().bHasSun != 0)
    {
        FLight Sun    = GetLightAt(0);
        float3 L      = normalize(GetSunDirection());
        float3 SunRad = GetLightColor(Sun).rgb * Sun.Intensity;

        float Shadow = SunShadow;

        // GGX against the variance-widened alpha, so distant water sparkles instead of going mirror flat.
        float3 H     = normalize(L + V);
        float  NdotL = saturate(dot(N, L));
        float  NdotH = saturate(dot(N, H));
        float  VdotH = saturate(dot(V, H));

        float A2    = SpecAlpha * SpecAlpha;
        float Denom = NdotH * NdotH * (A2 - 1.0) + 1.0;
        float NDF   = A2 / (PI * Denom * Denom);

        float Kg = SpecAlpha * 0.5;
        float G  = (NdotV / (NdotV * (1.0 - Kg) + Kg)) * (NdotL / (NdotL * (1.0 - Kg) + Kg));
        float Fs = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

        float Spec = (NDF * G * Fs) / max(4.0 * NdotV * NdotL, 1e-4);
        Color += SunRad * Spec * NdotL * Shadow * W.SSRSpecOpacity.z;

        float  WaveHeight = saturate((Input.WorldPos.y - WaterPlaneY) / max(W.WindAndWave.w, 0.05));
        float  BackLit    = pow(saturate(dot(V, -normalize(L + N * 0.5))), 4.0);
        Color += W.ShallowColor.rgb * SunRad * (BackLit * WaveHeight * Shadow * W.SSRSpecOpacity.z);
    }

    float Foam;
    {
        float2 Wind = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));
        float  Time = GetTime();
        float  Tile = max(W.DetailParams.z, 0.01);

        // Scroll layout follows tuxalin's foam, two layers at two tilings.
        float2 Pos = (Input.WorldPos.xz + V.xz * 0.1) * 0.5;
        float2 UV1 = (Pos + Time * 0.01   * Wind) * Tile;
        float2 UV2 = (Pos + Time * 0.015  * Wind) * -0.5 * Tile;
        float2 UV3 = (Pos + Time * 0.01   * Wind) * Tile * 0.25;
        float2 UV4 = (Pos - Time * 0.0045 * Wind) * -0.5 * Tile;

        float f1, f2, f3, f4;
        if (W.FoamTexture.IsValid())
        {
            FSampler FS = Samplers::LinearWrap;
            f1 = W.FoamTexture.Sample(FS, UV1).r;
            f2 = W.FoamTexture.Sample(FS, UV2).r;
            f3 = W.FoamTexture.Sample(FS, UV3).r;
            f4 = W.FoamTexture.Sample(FS, UV4).r;
        }
        else
        {
            f1 = WaterFbm(UV1); f2 = WaterFbm(UV2); f3 = WaterFbm(UV3); f4 = WaterFbm(UV4);
        }

        // Noise-perturbed depth ranges, as tuxalin does, so the foam edge looks natural.
        float  Shore  = max(W.FoamAbsorb.x, 0.0);
        float2 Ranges = float2(Shore, Shore * 2.5);
        Ranges.x += (WaterFbm(Input.WorldPos.xz * 0.1 + Wind * Time * 0.1) - 0.5) * Shore * 0.6;
        Ranges.y += (WaterFbm(Input.WorldPos.xz * 0.1 - Wind * Time * 0.1) - 0.5) * Shore * 0.6;
        Ranges = clamp(Ranges, 0.0, 10.0);
        float FoamEdge = max(Ranges.x, Ranges.y);

        float DeepFoam = lerp(f1 * 1.0  + f2 * 0.5, 0.0,      smoothstep(Ranges.x, FoamEdge, ColumnDepth));
        Foam           = lerp(f3 * 0.75 + f4 * 1.5, DeepFoam, smoothstep(0.0,      Ranges.x,  ColumnDepth));

        // Shoreline foam only over a real water column.
        Foam *= SubmergeMask;

        // Whitecaps where the surface pinches, since a height band would put foam in horizontal stripes.
        const float kFoamGain = 1.6;
        float CrestAmount = saturate(W.FoamAbsorb.y);
        float FoamBias    = lerp(0.25, 0.8, CrestAmount);
        float Crest       = saturate((FoamBias - Surface.Jacobian) * kFoamGain * 2.0);
        Foam += (f3 + f4 * 0.5) * Crest;

        Foam = saturate(Foam * W.DetailParams.w);
    }
    Color = lerp(Color, W.FoamColor.rgb * BodyLight, Foam);

    // Drawn after the atmosphere composite, so the haze and fog the ground already carries are applied here.
    if (Pass().AerialInScatter.IsValid())
    {
        Color = AerialPerspective_Apply(Color, ScreenUV, Input.WorldPos, Pass().AerialInScatter, Pass().AerialTransmittance,
                                        Pass().AerialRange, Pass().AerialIntensity);
    }
    float4 FogTerm = Fog_EvaluateForTranslucent(WorldToView(Input.WorldPos), Input.Position.xy);
    Color = Color * FogTerm.a + FogTerm.rgb;

    float Alpha = saturate(W.SSRSpecOpacity.w);
    return float4(Color, Alpha);
}
