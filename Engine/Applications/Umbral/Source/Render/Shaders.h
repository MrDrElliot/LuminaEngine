#pragma once

// Every shader embedded as source, so a normal engine boot never compiles any of it.
namespace Umbral::Shaders
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


    constexpr const char* kGroundCommon = R"SLANG(
        struct FGroundArgs
        {
            float2 Resolution;
            float2 CameraMin;
            float2 CameraSize;
            float  Time;
            float  Danger;
            float  Beat;
            float  Pad0;
        };
        FGroundArgs* Pass() { return GetArgs<FGroundArgs>(); }
    )SLANG";

    constexpr const char* kGroundPS = R"SLANG(
        [shader("fragment")]
        float4 GroundPS(FFullscreenOut Input) : SV_Target0
        {
            float2 World = Pass().CameraMin + Input.UV * Pass().CameraSize;
            float  Time = Pass().Time;

            float3 Color = float3(0.008, 0.009, 0.016);

            float Rock = Fbm(World * 0.0016);
            Color += float3(0.020, 0.018, 0.030) * pow(saturate(Rock), 2.2);

            float Veins = Fbm(World * 0.0045 + float2(Time * 0.008, -Time * 0.005));
            Color += float3(0.055, 0.012, 0.030) * pow(saturate(Veins - 0.42) * 1.7, 3.0);

            float2 Cell = abs(frac(World / 240.0) - 0.5);
            float  Grid = smoothstep(0.485, 0.5, max(Cell.x, Cell.y));
            Color += float3(0.016, 0.020, 0.034) * Grid;

            float Mist = Fbm(World * 0.0009 + float2(Time * 0.02, Time * 0.013));
            Color += float3(0.020, 0.024, 0.042) * pow(saturate(Mist), 2.0);

            Color = lerp(Color, Color * float3(1.6, 0.7, 0.7), Pass().Danger * 0.6);
            return float4(Color, 1.0);
        }
    )SLANG";


    constexpr const char* kLightCommon = R"SLANG(
        struct FLightInstance
        {
            float2 Center;
            float  Radius;
            float  Energy;
            float4 Color;
        };

        struct FLightArgs
        {
            FLightInstance* Lights;
            float2 NdcScale;
            float2 NdcOffset;
            float2 CameraMin;
            float  Time;
            float  Pad0;
        };
        FLightArgs* Pass() { return GetArgs<FLightArgs>(); }

        struct FLightInterp
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
    )SLANG";

    constexpr const char* kLightVS = R"SLANG(
        [shader("vertex")]
        FLightInterp LightVS(uint VertexID : SV_VulkanVertexID, uint InstanceID : SV_VulkanInstanceID)
        {
            FLightInstance Light = Pass().Lights[InstanceID];

            float2 Local = kCorners[VertexID] * Light.Radius;
            float2 World = Light.Center + Local - Pass().CameraMin;

            FLightInterp Output;
            Output.Position = float4(World * Pass().NdcScale + Pass().NdcOffset, 0.5, 1.0);
            Output.Local    = kCorners[VertexID];
            Output.Index    = InstanceID;
            return Output;
        }
    )SLANG";

    constexpr const char* kLightPS = R"SLANG(
        [shader("fragment")]
        float4 LightPS(FLightInterp Input) : SV_Target0
        {
            FLightInstance Light = Pass().Lights[Input.Index];

            float Distance = length(Input.Local);
            if (Distance > 1.0)
            {
                discard;
            }

            float Falloff = saturate(1.0 - Distance);
            Falloff = Falloff * Falloff * (3.0 - 2.0 * Falloff);
            Falloff *= Falloff;

            float Flicker = 0.94 + 0.06 * sin(Pass().Time * 21.0 + Light.Center.x * 0.03);
            return float4(Light.Color.rgb * Falloff * Light.Energy * Flicker, 1.0);
        }
    )SLANG";


    constexpr const char* kAgentCommon = R"SLANG(
        struct FAgentInstance
        {
            float2 Position;
            float  Radius;
            uint   Packed;
        };

        struct FAgentArgs
        {
            FAgentInstance* Agents;
            float2 NdcScale;
            float2 NdcOffset;
            float2 CameraMin;
            float2 Resolution;
            float2 PlayerWorld;
            float  Time;
            uint   LightID;
        };
        FAgentArgs* Pass() { return GetArgs<FAgentArgs>(); }

        struct FAgentInterp
        {
            float4 Position : SV_Position;
            float2 Local    : TEXCOORD0;
            nointerpolation uint Packed : TEXCOORD1;
            nointerpolation float2 ToLight : TEXCOORD2;
        };

        // Base radius, lobe count, lobe depth, vertical stretch. One table gives every kind its own outline.
        static const float4 kAgentProfiles[4] =
        {
            float4(0.80, 3.0, 0.06, 1.14),
            float4(0.74, 7.0, 0.22, 1.00),
            float4(0.96, 2.0, 0.14, 0.84),
            float4(0.70, 5.0, 0.12, 1.38),
        };

        static const float2 kCorners[6] =
        {
            float2(-1, -1), float2(1, -1), float2(1, 1),
            float2(-1, -1), float2(1,  1), float2(-1, 1),
        };

        static const float3 kAgentColors[4] =
        {
            float3(0.16, 0.22, 0.34),
            float3(0.62, 0.16, 0.20),
            float3(0.48, 0.20, 0.08),
            float3(0.26, 0.14, 0.52),
        };

        static const float3 kAgentEyes[4] =
        {
            float3(0.40, 0.75, 1.10),
            float3(1.30, 0.28, 0.22),
            float3(1.20, 0.50, 0.10),
            float3(0.70, 0.35, 1.40),
        };
    )SLANG";

    constexpr const char* kAgentVS = R"SLANG(
        [shader("vertex")]
        FAgentInterp AgentVS(uint VertexID : SV_VulkanVertexID, uint InstanceID : SV_VulkanInstanceID)
        {
            FAgentInstance Agent = Pass().Agents[InstanceID];

            float Extent = Agent.Radius * 1.9;
            float2 World = Agent.Position + kCorners[VertexID] * Extent - Pass().CameraMin;

            float2 Toward = Pass().PlayerWorld - Agent.Position;
            float Reach = max(length(Toward), 0.0001);

            FAgentInterp Output;
            Output.Position = float4(World * Pass().NdcScale + Pass().NdcOffset, 0.5, 1.0);
            Output.Local    = kCorners[VertexID] * 1.9;
            Output.Packed   = Agent.Packed;
            Output.ToLight  = Toward / Reach;
            return Output;
        }
    )SLANG";

    constexpr const char* kAgentPS = R"SLANG(
        [shader("fragment")]
        float4 AgentPS(FAgentInterp Input) : SV_Target0
        {
            uint Kind   = Input.Packed & 0xFFu;
            uint Health = (Input.Packed >> 8) & 0xFFu;
            uint Flash  = (Input.Packed >> 16) & 0xFFu;
            uint Seed   = (Input.Packed >> 24) & 0xFFu;

            float Phase = float(Seed) * 0.37;
            float4 Profile = kAgentProfiles[Kind & 3u];

            float2 P = Input.Local;
            P.x += sin(Pass().Time * 5.0 + Phase) * 0.07;
            P.y /= Profile.w;

            float Angle = atan2(P.y, P.x);
            float Reach = length(P);

            float Outline = Profile.x + Profile.z * sin(Angle * Profile.y + Pass().Time * 2.4 + Phase);

            // The brute grows a pair of horns; everything else is pure profile.
            if (Kind == 2u)
            {
                Outline += 0.26 * exp(-pow((abs(Angle) - 2.30) * 4.5, 2.0));
            }

            float Shape = Reach - Outline;
            float Body = 1.0 - smoothstep(-0.03, 0.05, Shape);
            if (Body < 0.004)
            {
                discard;
            }

            float2 ScreenUV = Input.Position.xy / Pass().Resolution;
            float3 Lit = SampleLevel0(Pass().LightID, ScreenUV).rgb;

            float3 Base = kAgentColors[Kind & 3u];
            float3 Eye  = kAgentEyes[Kind & 3u];

            // A dark mass that only reads where light lands, so the horde emerges out of the black.
            float3 Color = Base * (float3(0.045, 0.050, 0.072) + Lit * 1.35);

            // Rim toward the torch, which is what gives the silhouette its edge in the dark.
            float Facing = saturate(dot(normalize(P + 1e-5), Input.ToLight));
            float RimBand = smoothstep(-0.22, 0.02, Shape) * Body;
            Color += (Base * 2.4 + Eye * 0.5) * RimBand * pow(Facing, 1.6) * (0.35 + length(Lit) * 1.6);

            float EyeSpread = (Kind == 1u || Kind == 2u) ? 0.28 : 0.0;
            float2 EyeUp = float2(0.0, -0.20);
            float LeftEye = exp(-dot(P - EyeUp + float2(EyeSpread, 0.0), P - EyeUp + float2(EyeSpread, 0.0)) * 42.0);
            float RightEye = EyeSpread > 0.0
                ? exp(-dot(P - EyeUp - float2(EyeSpread, 0.0), P - EyeUp - float2(EyeSpread, 0.0)) * 42.0)
                : 0.0;

            float Blink = 0.75 + 0.25 * sin(Pass().Time * 3.1 + Phase * 2.7);
            Color += Eye * (LeftEye + RightEye) * Blink * (0.45 + float(Health) / 255.0 * 0.45);

            float FlashAmount = float(Flash) / 255.0;
            Color += float3(2.2, 1.9, 1.7) * FlashAmount * Body;

            return float4(Color * Body, Body);
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
            float2 CameraMin;
            float2 Resolution;
            float  Time;
            uint   LightID;
        };
        FQuadArgs* Pass() { return GetArgs<FQuadArgs>(); }

        static const uint KIND_RECT   = 0;
        static const uint KIND_DISC   = 1;
        static const uint KIND_RING   = 2;
        static const uint KIND_SPARK  = 3;
        static const uint KIND_GLOW   = 4;
        static const uint KIND_BLADE  = 5;
        static const uint KIND_RIBBON = 6;
        static const uint KIND_SIGIL  = 7;
        static const uint KIND_MOTE   = 8;
        static const uint KIND_BOLT   = 9;
        static const uint KIND_PANEL  = 10;

        struct FQuadInterp
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
            float Small = min(max(Instance.HalfSize.x, 0.5), max(Instance.HalfSize.y, 0.5));
            float Glow = max(Instance.Glow, 0.0);
            return min(2.0 + 12.0 * Glow, Small * (1.0 + Glow * 1.4));
        }

        // A crescent carved out of one disc by another, which is what stops a blade reading as a bar.
        float SdCrescent(float2 P, float Radius)
        {
            float Outer = length(P) - Radius;
            float Inner = length(P - float2(0.0, -Radius * 0.34)) - Radius * 0.88;
            return max(Outer, -Inner);
        }

        float SdStar(float2 P, float Radius)
        {
            float Diamond = (abs(P.x) + abs(P.y)) * 0.72 - Radius;
            float SpikeX = length(float2(P.x * 0.20, P.y)) - Radius * 0.30;
            float SpikeY = length(float2(P.x, P.y * 0.20)) - Radius * 0.30;
            return min(Diamond, min(SpikeX, SpikeY));
        }

        float SdTeardrop(float2 P, float2 Half)
        {
            float Head = length(float2((P.x - Half.x * 0.45) * 0.85, P.y)) - Half.y;
            float Taper = saturate((Half.x * 0.45 - P.x) / max(Half.x * 1.4, 1.0));
            float Tail = length(float2(P.x * 0.28, P.y / max(1.0 - Taper * 0.85, 0.15))) - Half.y * 0.9;
            return min(Head, Tail);
        }
    )SLANG";

    constexpr const char* kQuadVS = R"SLANG(
        [shader("vertex")]
        FQuadInterp QuadVS(uint VertexID : SV_VulkanVertexID, uint InstanceID : SV_VulkanInstanceID)
        {
            FQuadInstance Instance = Pass().Instances[InstanceID];

            float2 Expanded = Instance.HalfSize + GlowPixelsFor(Instance);
            float2 Local = kCorners[VertexID] * Expanded;

            float SinR = sin(Instance.Rotation);
            float CosR = cos(Instance.Rotation);
            float2 Rotated = float2(Local.x * CosR - Local.y * SinR, Local.x * SinR + Local.y * CosR);

            float2 World = Instance.Center + Rotated - Pass().CameraMin;

            FQuadInterp Output;
            Output.Position = float4(World * Pass().NdcScale + Pass().NdcOffset, 0.5, 1.0);
            Output.Local    = Local;
            Output.Index    = InstanceID;
            return Output;
        }
    )SLANG";

    constexpr const char* kQuadPS = R"SLANG(
        [shader("fragment")]
        float4 QuadPS(FQuadInterp Input) : SV_Target0
        {
            FQuadInstance Instance = Pass().Instances[Input.Index];

            float2 Half = max(Instance.HalfSize, float2(0.5, 0.5));
            float  Small = min(Half.x, Half.y);
            float  GlowPixels = GlowPixelsFor(Instance);

            if (Instance.Kind == KIND_GLOW)
            {
                float Radial = length(Input.Local / (Half + GlowPixels * 0.5));
                float Falloff = exp(-Radial * Radial * 4.0);
                float Churn = 0.75 + 0.25 * Fbm(Input.Local * 0.05 + Pass().Time * 1.4);
                float2 Bound = max(Half + GlowPixels, float2(1.0, 1.0));
                float Edge = 1.0 - smoothstep(0.70, 1.0, max(abs(Input.Local.x) / Bound.x, abs(Input.Local.y) / Bound.y));
                return float4(Instance.Color.rgb * Falloff * Churn, Falloff) * Edge * Instance.Color.a;
            }

            if (Instance.Kind == KIND_PANEL)
            {
                float Radius = Instance.CornerRadius * Small;
                float Shape = SdRoundBox(Input.Local, Half, Radius);
                float Width = max(fwidth(Shape), 1e-4);

                float Body = saturate(0.5 - Shape / Width);
                float Border = saturate(1.0 - abs(Shape + Instance.Param0) / max(Instance.Param1, 0.75));
                float Halo = exp(-max(Shape, 0.0) / max(GlowPixels * 0.55, 1.0)) * max(Instance.Glow, 0.0);

                float Gradient = saturate(0.5 - Input.Local.y / max(Half.y * 2.2, 1.0));
                float3 Fill = Instance.Accent.rgb * (0.45 + Gradient * 0.90);

                float Scan = 0.94 + 0.06 * sin((Input.Local.y + Pass().Time * 26.0) * 0.35);
                Fill *= Scan;

                float3 Color = Fill * Body + Instance.Color.rgb * (Border * 1.6 + Halo * 0.30);
                float Alpha = saturate(Body * Instance.Param2 + Border + Halo * 0.35);

                float2 Bound = max(Half + GlowPixels, float2(1.0, 1.0));
                float Edge = 1.0 - smoothstep(0.70, 1.0, max(abs(Input.Local.x) / Bound.x, abs(Input.Local.y) / Bound.y));
                return float4(Color * Edge, Alpha * Edge) * Instance.Color.a;
            }

            if (Instance.Kind == KIND_RIBBON)
            {
                float Extent = max(Half.x - Half.y, 0.0);
                float2 Nearest = float2(clamp(Input.Local.x, -Extent, Extent), 0.0);
                float Capsule = length(Input.Local - Nearest) - Half.y;
                float Width = max(fwidth(Capsule), 1e-4);
                float Core = saturate(0.5 - Capsule / Width);
                float Halo = exp(-max(Capsule, 0.0) / max(Half.y * 0.9, 1.0));
                return float4(Instance.Color.rgb * (Core + Halo * 0.6), saturate(Core + Halo * 0.4)) * Instance.Color.a;
            }

            float Distance;
            if (Instance.Kind == KIND_DISC || Instance.Kind == KIND_SPARK)
            {
                Distance = length(Input.Local / Half) * Small - Small;
            }
            else if (Instance.Kind == KIND_RING)
            {
                float Angle = atan2(Input.Local.y, Input.Local.x);
                float Ripple = 1.0 + 0.055 * sin(Angle * 9.0 + Pass().Time * 6.0)
                                   + 0.035 * sin(Angle * 17.0 - Pass().Time * 4.0);
                Distance = abs(length(Input.Local) - Small * Ripple) - max(Small * max(Instance.Param0, 0.02), 2.0);
            }
            else if (Instance.Kind == KIND_BLADE)
            {
                Distance = SdCrescent(Input.Local, Small * 0.86);
            }
            else if (Instance.Kind == KIND_MOTE)
            {
                float Spin = Pass().Time * 2.2 + Instance.Param1;
                float2 P = float2(Input.Local.x * cos(Spin) - Input.Local.y * sin(Spin),
                                  Input.Local.x * sin(Spin) + Input.Local.y * cos(Spin));
                Distance = SdStar(P, Small);
            }
            else if (Instance.Kind == KIND_BOLT)
            {
                Distance = SdTeardrop(Input.Local, Half);
            }
            else if (Instance.Kind == KIND_SIGIL)
            {
                float Core = length(Input.Local) - Small * 0.44;
                float Ring = abs(length(Input.Local) - Small * 0.86) - Small * 0.09;

                float Spin = Pass().Time * 1.3;
                float Angle = atan2(Input.Local.y, Input.Local.x) + Spin;
                float Petals = abs(frac(Angle / 2.0943951 + 0.5) - 0.5) * 2.0943951;
                float Notch = Petals * length(Input.Local) - Small * 0.16;
                Ring = max(Ring, -Notch);

                Distance = min(Core, Ring);
            }
            else
            {
                Distance = SdRoundBox(Input.Local, Half, Instance.CornerRadius * Small);
            }

            float Width = max(fwidth(Distance), 1e-4);
            float Fill = saturate(0.5 - Distance / Width);
            float Rim = exp(-abs(Distance) * 0.8);
            float Outer = exp(-max(Distance, 0.0) / max(GlowPixels * 0.4, 0.5));

            float2 Expanded = max(Half + GlowPixels, float2(1.0, 1.0));
            float2 Edge = abs(Input.Local) / Expanded;
            float Window = 1.0 - smoothstep(0.70, 1.0, max(Edge.x, Edge.y));

            float3 Base = Instance.Color.rgb;
            if (Instance.Param3 > 0.001)
            {
                float2 ScreenUV = Input.Position.xy / Pass().Resolution;
                float3 Lit = SampleLevel0(Pass().LightID, ScreenUV).rgb;
                Base = lerp(Base, Base * (float3(0.08, 0.09, 0.13) + Lit * 1.5), Instance.Param3);
            }

            float3 Emission = Base * Fill;
            Emission += Instance.Color.rgb * Rim * 0.30 * max(Instance.Glow, 0.10);
            Emission += Instance.Color.rgb * Outer * max(Instance.Glow, 0.0) * 0.14;

            float Coverage = saturate(Fill + Rim * 0.20 + Outer * 0.18 * max(Instance.Glow, 0.0));
            return float4(Emission * Window, Coverage * Window) * Instance.Color.a;
        }
    )SLANG";

    constexpr const char* kQuadAdditivePS = R"SLANG(
        [shader("fragment")]
        float4 QuadAdditivePS(FQuadInterp Input) : SV_Target0
        {
            FQuadInstance Instance = Pass().Instances[Input.Index];

            float2 Half = max(Instance.HalfSize, float2(0.5, 0.5));
            float  Small = min(Half.x, Half.y);
            float  GlowPixels = GlowPixelsFor(Instance);

            if (Instance.Kind == KIND_GLOW)
            {
                float Radial = length(Input.Local / (Half + GlowPixels * 0.5));
                float Falloff = exp(-Radial * Radial * 4.0);
                float2 Bound = max(Half + GlowPixels, float2(1.0, 1.0));
                float Edge = 1.0 - smoothstep(0.70, 1.0, max(abs(Input.Local.x) / Bound.x, abs(Input.Local.y) / Bound.y));
                return float4(Instance.Color.rgb * Falloff * Edge * Instance.Color.a, 0.0);
            }

            float Distance;
            if (Instance.Kind == KIND_RING)
            {
                float Angle = atan2(Input.Local.y, Input.Local.x);
                float Ripple = 1.0 + 0.055 * sin(Angle * 9.0 + Pass().Time * 6.0)
                                   + 0.035 * sin(Angle * 17.0 - Pass().Time * 4.0);
                Distance = abs(length(Input.Local) - Small * Ripple) - max(Small * max(Instance.Param0, 0.02), 2.0);
            }
            else if (Instance.Kind == KIND_BLADE)
            {
                Distance = SdCrescent(Input.Local, Small * 0.86);
            }
            else if (Instance.Kind == KIND_MOTE)
            {
                float Spin = Pass().Time * 2.2 + Instance.Param1;
                float2 P = float2(Input.Local.x * cos(Spin) - Input.Local.y * sin(Spin),
                                  Input.Local.x * sin(Spin) + Input.Local.y * cos(Spin));
                Distance = SdStar(P, Small);
            }
            else if (Instance.Kind == KIND_BOLT)
            {
                Distance = SdTeardrop(Input.Local, Half);
            }
            else
            {
                Distance = length(Input.Local / Half) * Small - Small;
            }

            float Width = max(fwidth(Distance), 1e-4);
            float Fill = saturate(0.5 - Distance / Width);
            float Rim = exp(-abs(Distance) * 0.8);
            float Outer = exp(-max(Distance, 0.0) / max(GlowPixels * 0.4, 0.5));

            float2 Expanded = max(Half + GlowPixels, float2(1.0, 1.0));
            float2 Edge = abs(Input.Local) / Expanded;
            float Window = 1.0 - smoothstep(0.70, 1.0, max(Edge.x, Edge.y));

            float3 Emission = Instance.Color.rgb * (Fill + Rim * 0.28 + Outer * 0.18);
            return float4(Emission * Window * Instance.Color.a, 0.0);
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
            float  Vignette;
            float  Time;
            float  Danger;
            float  Hurt;
            float  Fade;
            float  Pad0;
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
            float2 Centered = UV - 0.5;

            float Aberration = (0.0010 + Pass().Hurt * 0.008) * length(Centered);
            float2 Direction = normalize(Centered + 1e-6);

            float3 Scene;
            Scene.r = SampleLevel0(Pass().SceneID, UV + Direction * Aberration).r;
            Scene.g = SampleLevel0(Pass().SceneID, UV).g;
            Scene.b = SampleLevel0(Pass().SceneID, UV - Direction * Aberration).b;

            float3 Bloom = SampleLevel0(Pass().BloomID, UV).rgb;
            float3 Color = Scene + Bloom * Pass().BloomIntensity;

            Color += float3(0.60, 0.05, 0.06) * Pass().Hurt * 0.30;
            Color = AcesFilm(Color * Pass().Exposure);

            Color = lerp(Color, Color * float3(1.15, 0.80, 0.82), Pass().Danger * 0.5);

            float Vignette = 1.0 - (Pass().Vignette + Pass().Danger * 0.45) * dot(Centered, Centered) * 2.3;
            Color *= saturate(Vignette);

            Color += (Hash21(UV * Pass().Resolution + Pass().Time * 60.0) - 0.5) * 0.026;
            Color *= Pass().Fade;

            return float4(pow(max(Color, 0.0), 1.0 / 2.2), 1.0);
        }
    )SLANG";
}
