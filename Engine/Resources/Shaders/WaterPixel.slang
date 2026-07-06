#include "Includes/Water.slang"
#include "Includes/IBLRuntime.slang"

// Water surface shading

struct PSInput
{
    float4 Position : SV_Position;
    float3 WorldPos : WATER_WORLDPOS;
    float3 Normal   : WATER_NORMAL;
    float2 PlanarUV : WATER_UV;
    nointerpolation uint WaterIndex : WATER_INDEX;
};

// View space is +Z forward; visible geometry has positive view Z.
float SceneDistFromRaw(float2 UV, float RawDepth)
{
    return ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection()).z;
}

float SampleSceneDepthRaw(float2 UV)
{
    return gTextures2D[Pass().SceneDepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;
}

float3 SampleSceneColor(float2 UV)
{
    return gTextures2D[Pass().SceneColorIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], UV, 0.0).rgb;
}

// Cheap value-noise fbm used to break up procedural foam so it reads as wispy foam, not a solid white fill.
float WaterHash(float2 P)
{
    return frac(sin(dot(P, float2(127.1, 311.7))) * 43758.5453);
}

float WaterValueNoise(float2 P)
{
    float2 I = floor(P);
    float2 F = frac(P);
    F = F * F * (3.0 - 2.0 * F);
    float A = WaterHash(I);
    float B = WaterHash(I + float2(1.0, 0.0));
    float C = WaterHash(I + float2(0.0, 1.0));
    float D = WaterHash(I + float2(1.0, 1.0));
    return lerp(lerp(A, B, F.x), lerp(C, D, F.x), F.y);
}

float WaterFbm(float2 P)
{
    float V = 0.0;
    float A = 0.5;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        V += A * WaterValueNoise(P);
        P *= 2.0;
        A *= 0.5;
    }
    return V;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FGPUWater W = Pass().Waters[Input.WaterIndex];

    uint2  ScreenSize = Scene().ScreenSize.xy;
    float2 ScreenUV   = Input.Position.xy / float2(ScreenSize);

    float SceneRawDepth = SampleSceneDepthRaw(ScreenUV);
    float WaterRawDepth = Input.Position.z;
    if (SceneRawDepth > WaterRawDepth + 1e-6)
    {
        discard;
    }

    // --- Surface normal: Gerstner + optional scrolling detail normal map ---
    float3 N = normalize(Input.Normal);
    if (W.DetailNormalIndex != WATER_INVALID_TEX)
    {
        float  Tiling = W.DetailParams.x;
        float  Speed  = W.DetailParams.y;
        float2 Wind   = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));

        float2 UV0 = Input.PlanarUV * Tiling + Wind * (GetTime() * Speed);
        float2 UV1 = Input.PlanarUV * Tiling * 1.7 - Wind * (GetTime() * Speed * 0.7);
        float2 n0  = gTextures2D[W.DetailNormalIndex].Sample(gSamplers[SAMPLER_LINEAR_WRAP], UV0).rg * 2.0 - 1.0;
        float2 n1  = gTextures2D[W.DetailNormalIndex].Sample(gSamplers[SAMPLER_LINEAR_WRAP], UV1).rg * 2.0 - 1.0;
        float2 dn  = (n0 + n1) * W.WaveParams.w;

        // Perturb the world normal in its tangent plane (world X projected onto the surface as tangent).
        float3 T = normalize(float3(1.0, 0.0, 0.0) - N * N.x);
        float3 B = cross(N, T);
        N = normalize(N + (T * dn.x + B * dn.y));
    }

    float3 CameraPos = GetCameraPosition();
    float3 V     = normalize(CameraPos - Input.WorldPos);
    float  NdotV = saturate(dot(N, V));

    // Reconstruct the scene behind the water for both its distance (absorption) and its position
    bool   HasScene      = (SceneRawDepth > 0.0);
    float3 SceneViewPos  = ReconstructViewPos(ScreenUV, max(SceneRawDepth, 1e-5), GetInverseCameraProjection());
    float3 SceneWorldPos = mul(GetInverseCameraView(), float4(SceneViewPos, 1.0)).xyz;
    float  WaterViewDist = WorldToView(Input.WorldPos).z;
    float  SceneViewDist = HasScene ? SceneViewPos.z : GetFarPlane();
    float  ColumnDepth   = max(SceneViewDist - WaterViewDist, 0.0);
    float  DepthFade     = saturate(ColumnDepth / max(W.FoamAbsorb.z, 0.01));

    // Submerged only if the scene behind is inside the water's XZ box AND below its surface.
    float3 SceneLocal   = mul(W.WorldToWater, float4(SceneWorldPos, 1.0)).xyz;
    float  InsideXZ     = saturate((0.5 - max(abs(SceneLocal.x), abs(SceneLocal.z))) * 16.0);
    float  BelowSurface = saturate((Input.WorldPos.y - SceneWorldPos.y) * 4.0);
    float  SubmergeMask = HasScene ? (InsideXZ * BelowSurface) : 0.0;

    // --- Refraction: bend the scene UV by the surface normal, validated against foreground bleed ---
    float2 RefractOffset = N.xz * W.RefractReflect.x * (0.25 + 0.75 * DepthFade);
    float2 RefractUV     = ScreenUV + RefractOffset;
    float  RefrRaw       = SampleSceneDepthRaw(RefractUV);
    if (RefrRaw > WaterRawDepth + 1e-6 || any(saturate(RefractUV) != RefractUV))
    {
        RefractUV = ScreenUV;   // refracted sample is in front of the water (or off-screen) -> don't bend
    }
    float3 BehindColor = SampleSceneColor(RefractUV);


    float3 WaterColor = lerp(W.ShallowColor.rgb, W.DeepColor.rgb, DepthFade);
    float  Murk       = max(W.FoamAbsorb.w, 0.0);
    float  BedVis     = exp(-(2.0 + ColumnDepth) * Murk * (3.0 / max(W.FoamAbsorb.z, 0.1)));
    float3 Refracted  = lerp(W.DeepColor.rgb, BehindColor * WaterColor, BedVis);
    Refracted = lerp(WaterColor, Refracted, SubmergeMask);

    // --- Reflection: SSR ray-march against opaque depth, prefiltered-sky fallback ---
    float  Roughness = W.RefractReflect.z;
    float3 R         = reflect(-V, N);
    float3 SkyRefl   = SamplePrefilter(R, Roughness);
    float3 Reflection = SkyRefl;
    {
        float  StepCount = max(W.SSRSpecOpacity.y, 8.0);
        float  MaxDist   = W.SSRSpecOpacity.x;
        float3 StartV    = mul(GetCameraView(), float4(Input.WorldPos, 1.0)).xyz;
        float3 DirV      = normalize(mul((float3x3)GetCameraView(), R));
        if (DirV.z > 0.001)   // only march toward the scene (view space is +Z forward)
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
                    continue;   // sky: no geometry to reflect
                }
                float SampleDist = SceneDistFromRaw(Suv, SampleRaw);
                float RayDist    = Pv.z;
                if (RayDist > SampleDist && (RayDist - SampleDist) < StepLen * 3.0)
                {
                    Hit   = true;
                    HitUV = Suv;
                    break;
                }
            }

            if (Hit)
            {
                // Fade to the sky fallback near the screen border so reflections don't pop at the edge.
                float2 E        = abs(HitUV - 0.5) * 2.0;
                float  EdgeFade = smoothstep(0.0, 0.3, saturate(1.0 - max(E.x, E.y)));
                Reflection = lerp(SkyRefl, SampleSceneColor(HitUV), EdgeFade);
            }
        }
    }

    // --- Fresnel (Schlick, F0 ~ 0.02 for water) ---
    const float F0 = 0.02;
    float Fresnel = saturate(F0 + (1.0 - F0) * pow(1.0 - NdotV, W.RefractReflect.w)) * W.RefractReflect.y;

    float3 Color = lerp(Refracted, Reflection, Fresnel);

    // --- Sun specular + subsurface scattering ---
    if (LightData().bHasSun != 0)
    {
        FLight Sun    = GetLightAt(0);
        float3 L      = normalize(GetSunDirection());    // surface -> sun (to-light)
        float3 SunRad = GetLightColor(Sun).rgb * Sun.Intensity;

        // Blinn-Phong sun glint (cascade shadowing on water is a deferred enhancement).
        float  NdotH = saturate(dot(N, normalize(L + V)));
        float  Shine = lerp(2048.0, 16.0, saturate(Roughness));
        float  Spec  = pow(NdotH, Shine) * (Shine + 8.0) / (8.0 * PI);
        Color += SunRad * Spec * W.SSRSpecOpacity.z;

        // Subsurface scattering
        float  MeanY      = mul(W.WaterToWorld, float4(0.0, 0.0, 0.0, 1.0)).y;
        float  WaveHeight = saturate((Input.WorldPos.y - MeanY) / max(W.WindAndWave.w, 0.05));
        float  BackLit    = pow(saturate(dot(V, -normalize(L + N * 0.5))), 4.0);
        Color += W.ShallowColor.rgb * SunRad * (BackLit * WaveHeight * W.SSRSpecOpacity.z);
    }

    // --- Foam
    float Foam;
    {
        float2 Wind = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));
        float  Time = GetTime();
        float  Tile = max(W.DetailParams.z, 0.01);

        // tuxalin: position = (surface.xz + eye.xz*0.1)*0.5; four scrolled tex coords (two layers, two tilings).
        float2 Pos = (Input.WorldPos.xz + V.xz * 0.1) * 0.5;
        float2 UV1 = (Pos + Time * 0.01   * Wind) * Tile;
        float2 UV2 = (Pos + Time * 0.015  * Wind) * -0.5 * Tile;
        float2 UV3 = (Pos + Time * 0.01   * Wind) * Tile * 0.25;
        float2 UV4 = (Pos - Time * 0.0045 * Wind) * -0.5 * Tile;

        float f1, f2, f3, f4;
        if (W.FoamTextureIndex != WATER_INVALID_TEX)
        {
            SamplerState FS = gSamplers[SAMPLER_LINEAR_WRAP];
            f1 = gTextures2D[W.FoamTextureIndex].Sample(FS, UV1).r;
            f2 = gTextures2D[W.FoamTextureIndex].Sample(FS, UV2).r;
            f3 = gTextures2D[W.FoamTextureIndex].Sample(FS, UV3).r;
            f4 = gTextures2D[W.FoamTextureIndex].Sample(FS, UV4).r;
        }
        else
        {
            f1 = WaterFbm(UV1); f2 = WaterFbm(UV2); f3 = WaterFbm(UV3); f4 = WaterFbm(UV4);
        }

        // Noise-perturbed depth ranges (tuxalin perturbs ranges.x/y with snoise) so the foam edge is natural.
        float  Shore  = max(W.FoamAbsorb.x, 0.0);
        float2 Ranges = float2(Shore, Shore * 2.5);
        Ranges.x += (WaterFbm(Input.WorldPos.xz * 0.1 + Wind * Time * 0.1) - 0.5) * Shore * 0.6;
        Ranges.y += (WaterFbm(Input.WorldPos.xz * 0.1 - Wind * Time * 0.1) - 0.5) * Shore * 0.6;
        Ranges = clamp(Ranges, 0.0, 10.0);
        float FoamEdge = max(Ranges.x, Ranges.y);

        // FoamColor(): deep band foam->0 over [Ranges.x, FoamEdge]; shore band shoreFoam->deepFoam over [0, Ranges.x].
        float DeepFoam = lerp(f1 * 1.0  + f2 * 0.5, 0.0,      smoothstep(Ranges.x, FoamEdge, ColumnDepth));
        Foam           = lerp(f3 * 0.75 + f4 * 1.5, DeepFoam, smoothstep(0.0,      Ranges.x,  ColumnDepth));

        // Shoreline foam only where genuinely submerged (a real water column over a bed).
        Foam *= SubmergeMask;

        // Wave-crest foam: above a height threshold, add foam scaled by crest height (CrestFoamAmount sets it).
        float MeanY  = mul(W.WaterToWorld, float4(0.0, 0.0, 0.0, 1.0)).y;
        float Height = Input.WorldPos.y - MeanY;
        float Crest  = W.WindAndWave.w * (1.0 - saturate(W.FoamAbsorb.y));
        if (Height > Crest + 0.0001)
        {
            float Amount = saturate((Height - Crest) / max(W.WindAndWave.w, 0.05)) * 0.5;
            Foam += (f3 + f4 * 0.5) * Amount;
        }

        Foam = saturate(Foam * W.DetailParams.w);   // FoamIntensity
    }
    Color = lerp(Color, W.FoamColor.rgb, Foam);

    float Alpha = saturate(W.SSRSpecOpacity.w);
    return float4(Color, Alpha);
}
