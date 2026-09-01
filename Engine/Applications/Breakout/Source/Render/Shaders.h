#pragma once

// Every shader embedded as source, so a normal engine boot never compiles any of it.
namespace Breakout::Shaders
{
    constexpr const char* kPrelude = R"SLANG(
        struct FRHIRoot { uint64_t Args; };
        [[vk::push_constant]] FRHIRoot gRHI;
        Ptr<T> GetArgs<T>() { return (T*)gRHI.Args; }

        [[vk::binding(0, 0)]] SamplerState gSamplers[];
        [[vk::binding(1, 0)]] Texture2D    gTextures2D[];

        static const uint SAMPLER_LINEAR_CLAMP = 1;

        float4 SampleLevel0(uint TextureID, float2 UV)
        {
            return gTextures2D[TextureID].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], UV, 0.0);
        }

        float Hash21(float2 P)
        {
            P = frac(P * float2(123.34, 456.21));
            P += dot(P, P + 45.32);
            return frac(P.x * P.y);
        }

        float ValueNoise(float2 P)
        {
            float2 I = floor(P);
            float2 F = frac(P);
            F = F * F * (3.0 - 2.0 * F);

            float A = Hash21(I);
            float B = Hash21(I + float2(1, 0));
            float C = Hash21(I + float2(0, 1));
            float D = Hash21(I + float2(1, 1));
            return lerp(lerp(A, B, F.x), lerp(C, D, F.x), F.y);
        }

        float Fbm(float2 P)
        {
            float Sum = 0.0;
            float Amplitude = 0.5;
            for (int i = 0; i < 5; ++i)
            {
                Sum += ValueNoise(P) * Amplitude;
                P = P * 2.03 + 17.3;
                Amplitude *= 0.5;
            }
            return Sum;
        }

        struct FFullscreenOut
        {
            float4 Position : SV_Position;
            float2 UV       : TEXCOORD0;
        };
    )SLANG";

    // One entry point per compile, since the compiler concatenates the SPIR-V of every entry point it finds.
    constexpr const char* kFullscreenVS = R"SLANG(
        [shader("vertex")]
        FFullscreenOut FullscreenVS(uint VertexID : SV_VertexID)
        {
            FFullscreenOut Output;
            Output.UV = float2((VertexID << 1) & 2, VertexID & 2);
            Output.Position = float4(Output.UV * 2.0 - 1.0, 0.5, 1.0);
            return Output;
        }
    )SLANG";


    constexpr const char* kBackgroundCommon = R"SLANG(
        struct FBackgroundArgs
        {
            float2 Resolution;
            float2 FieldOrigin;
            float2 FieldSize;
            float  Time;
            float  Flash;
            float  Level;
            float  Beat;
            float  Danger;
            float  Progress;
            float  Fever;
            float  Pad0;
        };
        FBackgroundArgs* Pass() { return GetArgs<FBackgroundArgs>(); }

        float StarField(float2 UV, float Time)
        {
            float2 Grid = UV * 160.0;
            float2 Cell = floor(Grid);
            float  Seed = Hash21(Cell);
            if (Seed < 0.986)
            {
                return 0.0;
            }

            float2 Center = Cell + 0.5 + (float2(Hash21(Cell + 3.1), Hash21(Cell + 7.7)) - 0.5) * 0.7;
            float  Distance = length(Grid - Center);
            float  Twinkle = 0.55 + 0.45 * sin(Time * 2.1 + Seed * 90.0);
            return exp(-Distance * 8.0) * Twinkle;
        }
    )SLANG";

    constexpr const char* kBackgroundPS = R"SLANG(
        [shader("fragment")]
        float4 BackgroundPS(FFullscreenOut Input) : SV_Target0
        {
            float2 Resolution = Pass().Resolution;
            float2 Pixel = Input.UV * Resolution;
            float2 Centered = (Input.UV - 0.5) * float2(Resolution.x / max(Resolution.y, 1.0), 1.0);
            float  Time = Pass().Time;

            float Beat = Pass().Beat;
            float Danger = Pass().Danger;
            float Progress = Pass().Progress;
            float Fever = Pass().Fever;

            float3 Deep = lerp(float3(0.022, 0.026, 0.062), float3(0.075, 0.010, 0.022), Danger);
            float3 Color = lerp(Deep, float3(0.003, 0.004, 0.013), saturate(length(Centered) * 1.15));

            float Nebula = Fbm(Centered * 2.1 + float2(Time * 0.021, Time * 0.013));
            float3 NebulaTint = lerp(float3(0.115, 0.035, 0.260), float3(0.320, 0.030, 0.060), Danger);
            NebulaTint = lerp(NebulaTint, float3(0.420, 0.120, 0.360), Fever);
            Color += NebulaTint * pow(saturate(Nebula), 2.6) * (1.05 + Beat * 0.55);

            float Nebula2 = Fbm(Centered * 3.3 - float2(Time * 0.017, Time * 0.008) + 11.0);
            Color += float3(0.020, 0.150, 0.230) * pow(saturate(Nebula2), 3.2) * (0.85 + Progress * 0.5);

            Color += StarField(Input.UV, Time) * float3(0.75, 0.85, 1.0) * (0.9 + Beat * 0.5);

            float2 FieldHalf = Pass().FieldSize * 0.5;
            float2 FieldCenter = Pass().FieldOrigin + FieldHalf;

            float2 Q = abs(Pixel - FieldCenter) - FieldHalf;
            float  Border = length(max(Q, 0.0)) + min(max(Q.x, Q.y), 0.0);
            float  Inside = 1.0 - saturate(Border * 0.5 + 0.5);

            float2 Local = (Pixel - Pass().FieldOrigin) / max(Pass().FieldSize, float2(1.0, 1.0));
            float2 GridLine = abs(frac(Local * float2(24.0, 14.0)) - 0.5);
            float  Grid = smoothstep(0.47, 0.5, max(GridLine.x, GridLine.y));

            Color = lerp(Color, Color * 0.42, Inside * 0.9);

            float GridSweep = saturate(1.0 - abs(frac(Local.y * 14.0 - Time * 0.35) - 0.5) * 4.0);
            float3 GridTint = lerp(float3(0.05, 0.12, 0.22), float3(0.26, 0.05, 0.09), Danger);
            Color += GridTint * Grid * Inside * (0.32 + GridSweep * 0.55 + Beat * 0.30);

            float Frame = exp(-abs(Border) * 0.22) * 0.9;
            float Hue = Pass().Level * 0.7;
            float3 FrameColor = float3(0.35 + 0.35 * sin(Hue), 0.55 + 0.25 * sin(Hue + 2.1), 1.0);
            FrameColor = lerp(FrameColor, float3(1.0, 0.20, 0.24), Danger);
            Color += FrameColor * Frame * (0.55 + Pass().Flash * 2.0 + Beat * 0.5 + Danger * 0.6);

            Color *= 0.985 + 0.015 * sin(Pixel.y * 3.14159 + Time * 2.0);
            Color += float3(0.10, 0.02, 0.09) * Fever * (0.5 + Beat * 0.8) * Inside;
            return float4(Color, 1.0);
        }
    )SLANG";


    constexpr const char* kQuadCommon = R"SLANG(
        struct FQuadInstance
        {
            float2 Center;
            float2 HalfSize;
            float4 Color;
            float4 Accent;
            float  Rotation;
            float  CornerRadius;
            float  Glow;
            float  Param0;
            uint   Kind;
            float  Param1;
            float  Param2;
            float  Param3;
        };

        struct FQuadArgs
        {
            FQuadInstance* Instances;
            float2 NdcScale;
            float2 NdcOffset;
            float2 UnitsToPixels;
            float  Time;
            float  Pad0;
        };
        FQuadArgs* Pass() { return GetArgs<FQuadArgs>(); }

        static const uint KIND_RECT  = 0;
        static const uint KIND_BRICK = 1;
        static const uint KIND_DISC  = 2;
        static const uint KIND_RING  = 3;
        static const uint KIND_SPARK = 4;
        static const uint KIND_GLOW  = 5;
        static const uint KIND_BOLT   = 6;
        static const uint KIND_RIBBON = 7;

        static const uint BRICK_NORMAL     = 0;
        static const uint BRICK_REINFORCED = 1;
        static const uint BRICK_EXPLOSIVE  = 2;
        static const uint BRICK_STEEL      = 3;
        static const uint BRICK_MYSTERY    = 4;

        struct FQuadInterpolants
        {
            float4 Position : SV_Position;
            float2 Local    : TEXCOORD0;
            nointerpolation uint Index : TEXCOORD1;
        };

        static const float2 kCorners[6] =
        {
            float2(-1, -1), float2(1, -1), float2(1, 1),
            float2(-1, -1), float2(1,  1), float2(-1, 1),
        };

        float SdRoundBox(float2 P, float2 Half, float Radius)
        {
            float2 Q = abs(P) - Half + Radius;
            return length(max(Q, 0.0)) + min(max(Q.x, Q.y), 0.0) - Radius;
        }

        float GlowPixelsFor(FQuadInstance Instance)
        {
            return 3.0 + 14.0 * max(Instance.Glow, 0.0);
        }

        float4 ShadeQuad(FQuadInstance Instance, float2 Local)
        {
            float2 Half = max(Instance.HalfSize * Pass().UnitsToPixels, float2(0.5, 0.5));
            float  Small = min(Half.x, Half.y);
            float  GlowPixels = GlowPixelsFor(Instance);

            if (Instance.Kind == KIND_GLOW)
            {
                float Radial = length(Local / (Half + GlowPixels * 0.5));
                float Falloff = exp(-Radial * Radial * 4.5);
                return float4(Instance.Color.rgb * Falloff, Falloff) * Instance.Color.a;
            }

            if (Instance.Kind == KIND_RIBBON)
            {
                float Extent = max(Half.x - Half.y, 0.0);
                float2 Nearest = float2(clamp(Local.x, -Extent, Extent), 0.0);
                float Capsule = length(Local - Nearest) - Half.y;

                float Width = max(fwidth(Capsule), 1e-4);
                float Core = saturate(0.5 - Capsule / Width);
                float Halo = exp(-max(Capsule, 0.0) / max(Half.y * 0.9, 1.0));

                float3 Tint = lerp(Instance.Accent.rgb, Instance.Color.rgb, Instance.Param0);
                return float4(Tint * (Core + Halo * 0.55), saturate(Core + Halo * 0.4)) * Instance.Color.a;
            }

            if (Instance.Kind == KIND_BOLT)
            {
                float Core = exp(-pow(Local.x / max(Half.x, 0.5), 2.0) * 3.0);
                float Along = saturate(1.0 - abs(Local.y) / max(Half.y, 0.5));
                float Head = exp(-pow((Local.y + Half.y) / max(Half.y * 0.35, 0.5), 2.0));
                float Shape = Core * (Along * 0.7 + Head * 0.9);
                float3 Bolt = lerp(Instance.Color.rgb, Instance.Accent.rgb, Head);
                return float4(Bolt * Shape, saturate(Shape)) * Instance.Color.a;
            }

            float Distance;
            if (Instance.Kind == KIND_DISC || Instance.Kind == KIND_SPARK)
            {
                Distance = length(Local / Half) * Small - Small;
            }
            else if (Instance.Kind == KIND_RING)
            {
                Distance = abs(length(Local) - Small) - max(Small * Instance.Param0, 1.5);
            }
            else
            {
                Distance = SdRoundBox(Local, Half, Instance.CornerRadius * Small);
            }

            float Width = max(fwidth(Distance), 1e-4);
            float Fill = saturate(0.5 - Distance / Width);

            float3 Base = Instance.Color.rgb;

            if (Instance.Kind == KIND_BRICK)
            {
                float Vertical = saturate(Local.y / Half.y * 0.5 + 0.5);
                Base = lerp(Instance.Color.rgb * 1.25, Instance.Accent.rgb, Vertical * 0.85);

                float InnerEdge = SdRoundBox(Local, Half - 4.0, Instance.CornerRadius * Small);
                Base += Instance.Color.rgb * saturate(1.0 - abs(InnerEdge) / 2.0) * 0.55;

                float Damage = Instance.Param0;
                if (Damage > 0.01)
                {
                    float Crack = Fbm(Local * 0.22 + Instance.Param1);
                    float Mask = smoothstep(0.56 - Damage * 0.28, 0.63 - Damage * 0.28, Crack);
                    Base *= lerp(1.0, 0.26, Mask * Damage);
                }

                float Sheen = exp(-pow(saturate((Local.y + Half.y) / max(Half.y, 1.0) * 0.5), 2.0) * 9.0);
                Base += Instance.Color.rgb * Sheen * 0.35;

                uint BrickKind = uint(Instance.Param2 + 0.5);
                if (BrickKind == BRICK_EXPLOSIVE)
                {
                    float Core = exp(-dot(Local / Half, Local / Half) * 5.0);
                    float Pulse = 0.6 + 0.4 * sin(Pass().Time * 9.0 + Instance.Param1);
                    Base += float3(1.6, 0.55, 0.12) * Core * Pulse;

                    float Ring = abs(length(Local / Half) - 0.55);
                    Base += float3(1.2, 0.35, 0.05) * saturate(1.0 - Ring * 9.0) * Pulse * 0.7;
                }
                else if (BrickKind == BRICK_STEEL)
                {
                    float Rivets = 0.0;
                    Rivets += saturate(1.0 - length(Local - float2(-Half.x + 10.0, 0.0)) / 4.0);
                    Rivets += saturate(1.0 - length(Local - float2( Half.x - 10.0, 0.0)) / 4.0);
                    Base += float3(0.9, 0.95, 1.1) * Rivets * 0.6;

                    float Brushed = 0.5 + 0.5 * sin(Local.y * 2.2 + Local.x * 0.35);
                    Base *= 0.85 + Brushed * 0.30;
                }
                else if (BrickKind == BRICK_MYSTERY)
                {
                    float Sweep = frac(Local.x / max(Half.x * 2.0, 1.0) - Pass().Time * 0.55);
                    float3 Rainbow = 0.5 + 0.5 * cos(6.2831853 * (Sweep + float3(0.0, 0.33, 0.67)));
                    Base = lerp(Base, Rainbow * 1.1, 0.55);
                }
            }
            else if (Instance.Kind == KIND_RECT)
            {
                float Vertical = saturate(Local.y / Half.y * 0.5 + 0.5);
                Base = lerp(Instance.Color.rgb, lerp(Instance.Accent.rgb, Instance.Color.rgb, 0.5), Vertical * 0.6);
            }
            else if (Instance.Kind == KIND_DISC)
            {
                float Radial = saturate(1.0 - length(Local / Half));
                Base = lerp(Instance.Accent.rgb, Instance.Color.rgb, pow(Radial, 0.55));
            }

            float Rim = exp(-abs(Distance) * 0.8);
            float Outer = exp(-max(Distance, 0.0) / max(GlowPixels * 0.38, 0.5));

            float3 Emission = Base * Fill;
            Emission += Instance.Color.rgb * Rim * 0.45 * max(Instance.Glow, 0.12);
            Emission += Instance.Color.rgb * Outer * max(Instance.Glow, 0.0) * 0.20;

            float Coverage = saturate(Fill + Rim * 0.25 + Outer * 0.25 * max(Instance.Glow, 0.0));
            return float4(Emission, Coverage) * Instance.Color.a;
        }
    )SLANG";

    constexpr const char* kQuadVS = R"SLANG(
        [shader("vertex")]
        FQuadInterpolants QuadVS(uint VertexID : SV_VulkanVertexID, uint InstanceID : SV_VulkanInstanceID)
        {
            FQuadInstance Instance = Pass().Instances[InstanceID];

            float2 Expanded = Instance.HalfSize * Pass().UnitsToPixels + GlowPixelsFor(Instance);
            float2 Local = kCorners[VertexID] * Expanded;

            float SinR = sin(Instance.Rotation);
            float CosR = cos(Instance.Rotation);
            float2 Rotated = float2(Local.x * CosR - Local.y * SinR, Local.x * SinR + Local.y * CosR);

            float2 CenterPixels = Instance.Center * Pass().UnitsToPixels;
            float2 Ndc = (CenterPixels + Rotated) * Pass().NdcScale + Pass().NdcOffset;

            FQuadInterpolants Output;
            Output.Position = float4(Ndc, 0.5, 1.0);
            Output.Local    = Local;
            Output.Index    = InstanceID;
            return Output;
        }
    )SLANG";

    constexpr const char* kQuadPS = R"SLANG(
        [shader("fragment")]
        float4 QuadPS(FQuadInterpolants Input) : SV_Target0
        {
            float4 Shaded = ShadeQuad(Pass().Instances[Input.Index], Input.Local);
            if (Shaded.a < 0.0015)
            {
                discard;
            }
            return Shaded;
        }
    )SLANG";

    constexpr const char* kQuadAdditivePS = R"SLANG(
        [shader("fragment")]
        float4 QuadAdditivePS(FQuadInterpolants Input) : SV_Target0
        {
            float4 Shaded = ShadeQuad(Pass().Instances[Input.Index], Input.Local);
            return float4(Shaded.rgb, 0.0);
        }
    )SLANG";


    constexpr const char* kBloomCommon = R"SLANG(
        struct FBloomArgs
        {
            uint   SourceID;
            uint   bFirstPass;
            float2 SourceTexelSize;
            float  Threshold;
            float  Radius;
            float  Intensity;
            float  Pad0;
        };
        FBloomArgs* Pass() { return GetArgs<FBloomArgs>(); }

        float KarisWeight(float3 Color)
        {
            return 1.0 / (1.0 + dot(Color, float3(0.2126, 0.7152, 0.0722)));
        }
    )SLANG";

    constexpr const char* kDownsamplePS = R"SLANG(
        [shader("fragment")]
        float4 DownsamplePS(FFullscreenOut Input) : SV_Target0
        {
            float2 UV = Input.UV;
            float2 Texel = Pass().SourceTexelSize;
            uint   Source = Pass().SourceID;

            float3 A = SampleLevel0(Source, UV + Texel * float2(-2, -2)).rgb;
            float3 B = SampleLevel0(Source, UV + Texel * float2( 0, -2)).rgb;
            float3 C = SampleLevel0(Source, UV + Texel * float2( 2, -2)).rgb;
            float3 D = SampleLevel0(Source, UV + Texel * float2(-1, -1)).rgb;
            float3 E = SampleLevel0(Source, UV + Texel * float2( 1, -1)).rgb;
            float3 F = SampleLevel0(Source, UV + Texel * float2(-2,  0)).rgb;
            float3 G = SampleLevel0(Source, UV).rgb;
            float3 H = SampleLevel0(Source, UV + Texel * float2( 2,  0)).rgb;
            float3 I = SampleLevel0(Source, UV + Texel * float2(-1,  1)).rgb;
            float3 J = SampleLevel0(Source, UV + Texel * float2( 1,  1)).rgb;
            float3 K = SampleLevel0(Source, UV + Texel * float2(-2,  2)).rgb;
            float3 L = SampleLevel0(Source, UV + Texel * float2( 0,  2)).rgb;
            float3 M = SampleLevel0(Source, UV + Texel * float2( 2,  2)).rgb;

            float3 Inner = (D + E + I + J) * 0.25;
            float3 Group0 = (A + B + F + G) * 0.25;
            float3 Group1 = (B + C + G + H) * 0.25;
            float3 Group2 = (F + G + K + L) * 0.25;
            float3 Group3 = (G + H + L + M) * 0.25;

            float3 Result;
            if (Pass().bFirstPass != 0)
            {
                float W0 = KarisWeight(Inner) * 0.5;
                float W1 = KarisWeight(Group0) * 0.125;
                float W2 = KarisWeight(Group1) * 0.125;
                float W3 = KarisWeight(Group2) * 0.125;
                float W4 = KarisWeight(Group3) * 0.125;

                Result = (Inner * W0 + Group0 * W1 + Group1 * W2 + Group2 * W3 + Group3 * W4)
                       / max(W0 + W1 + W2 + W3 + W4, 1e-5);

                float Luminance = dot(Result, float3(0.2126, 0.7152, 0.0722));
                Result *= smoothstep(Pass().Threshold * 0.55, Pass().Threshold, Luminance);
            }
            else
            {
                Result = Inner * 0.5 + (Group0 + Group1 + Group2 + Group3) * 0.125;
            }

            return float4(Result, 1.0);
        }
    )SLANG";

    constexpr const char* kUpsamplePS = R"SLANG(
        [shader("fragment")]
        float4 UpsamplePS(FFullscreenOut Input) : SV_Target0
        {
            float2 UV = Input.UV;
            float2 Offset = Pass().SourceTexelSize * Pass().Radius;
            uint   Source = Pass().SourceID;

            float3 Sum = SampleLevel0(Source, UV + float2(-Offset.x,  Offset.y)).rgb;
            Sum += SampleLevel0(Source, UV + float2(0.0,  Offset.y)).rgb * 2.0;
            Sum += SampleLevel0(Source, UV + float2( Offset.x,  Offset.y)).rgb;
            Sum += SampleLevel0(Source, UV + float2(-Offset.x, 0.0)).rgb * 2.0;
            Sum += SampleLevel0(Source, UV).rgb * 4.0;
            Sum += SampleLevel0(Source, UV + float2( Offset.x, 0.0)).rgb * 2.0;
            Sum += SampleLevel0(Source, UV + float2(-Offset.x, -Offset.y)).rgb;
            Sum += SampleLevel0(Source, UV + float2(0.0, -Offset.y)).rgb * 2.0;
            Sum += SampleLevel0(Source, UV + float2( Offset.x, -Offset.y)).rgb;

            return float4(Sum * (1.0 / 16.0) * Pass().Intensity, 1.0);
        }
    )SLANG";


    constexpr const char* kCompositeCommon = R"SLANG(
        struct FCompositeArgs
        {
            uint   SceneID;
            uint   BloomID;
            float2 Resolution;
            float  BloomIntensity;
            float  Exposure;
            float  Chroma;
            float  Vignette;
            float  Time;
            float  Flash;
            float  Fade;
            float  Danger;
            float  Fire;
            float  Beat;
            float  Fever;
            float  Pad0;
            float4 Waves[4];
        };
        FCompositeArgs* Pass() { return GetArgs<FCompositeArgs>(); }

        float3 AcesFilm(float3 X)
        {
            const float A = 2.51;
            const float B = 0.03;
            const float C = 2.43;
            const float D = 0.59;
            const float E = 0.14;
            return saturate((X * (A * X + B)) / (X * (C * X + D) + E));
        }
    )SLANG";

    constexpr const char* kCompositePS = R"SLANG(
        [shader("fragment")]
        float4 CompositePS(FFullscreenOut Input) : SV_Target0
        {
            float2 UV = Input.UV;
            float  Aspect = Pass().Resolution.x / max(Pass().Resolution.y, 1.0);

            for (int Index = 0; Index < 4; ++Index)
            {
                float4 Wave = Pass().Waves[Index];
                if (Wave.w <= 0.0)
                {
                    continue;
                }

                float2 Delta = (UV - Wave.xy) * float2(Aspect, 1.0);
                float  Radius = length(Delta) + 1e-5;
                float  Band = exp(-pow((Radius - Wave.z) / 0.035, 2.0));
                UV += (Delta / Radius) * Band * Wave.w * 0.035 / float2(Aspect, 1.0);
            }

            float2 Centered = UV - 0.5;

            float Aberration = (0.0012 + Pass().Chroma * 0.010) * length(Centered);
            float2 Direction = normalize(Centered + 1e-6);

            float3 Scene;
            Scene.r = SampleLevel0(Pass().SceneID, UV + Direction * Aberration).r;
            Scene.g = SampleLevel0(Pass().SceneID, UV).g;
            Scene.b = SampleLevel0(Pass().SceneID, UV - Direction * Aberration).b;

            float3 Bloom = SampleLevel0(Pass().BloomID, UV).rgb;
            float3 Color = Scene + Bloom * Pass().BloomIntensity;

            Color += float3(0.35, 0.55, 1.0) * Pass().Flash * 0.35;
            Color += float3(1.00, 0.35, 0.08) * Pass().Fire * 0.22;
            Color += float3(1.00, 0.10, 0.14) * Pass().Danger * (0.05 + Pass().Beat * 0.10);
            Color = AcesFilm(Color * Pass().Exposure);

            Color = lerp(Color, Color * float3(1.12, 0.86, 0.88), Pass().Danger * 0.55);

            float Fever = Pass().Fever;
            if (Fever > 0.001)
            {
                float Luma = dot(Color, float3(0.2126, 0.7152, 0.0722));
                Color = lerp(Color, lerp(float3(Luma, Luma, Luma), Color, 1.55) * float3(1.10, 0.94, 1.16), Fever);
            }
            Color *= saturate(1.0 - (Pass().Vignette + Pass().Danger * 0.35) * dot(Centered, Centered) * 2.1);
            Color += (Hash21(UV * Pass().Resolution + Pass().Time * 60.0) - 0.5) * 0.022;
            Color *= Pass().Fade;

            return float4(pow(max(Color, 0.0), 1.0 / 2.2), 1.0);
        }
    )SLANG";
}
