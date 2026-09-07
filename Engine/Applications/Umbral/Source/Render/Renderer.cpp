#include "Renderer.h"

#include "Font.h"
#include "Shaders.h"
#include "Log/Log.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"

namespace Umbral
{
    namespace
    {
        constexpr EFormat kSceneFormat = EFormat::RGBA16_FLOAT;

        struct FGroundArgs
        {
            FVector2 Resolution;
            FVector2 CameraMin;
            FVector2 CameraSize;
            float    Time;
            float    Danger;
            float    Beat;
            float    Pad0;
        };

        struct FLightArgs
        {
            RHI::GPUPtr Lights;
            FVector2    NdcScale;
            FVector2    NdcOffset;
            FVector2    CameraMin;
            float       Time;
            float       Pad0;
        };

        struct FAgentArgs
        {
            RHI::GPUPtr Agents;
            FVector2    NdcScale;
            FVector2    NdcOffset;
            FVector2    CameraMin;
            FVector2    Resolution;
            FVector2    PlayerWorld;
            float       Time;
            uint32      LightID;
        };

        struct FQuadArgs
        {
            RHI::GPUPtr Instances;
            FVector2    NdcScale;
            FVector2    NdcOffset;
            FVector2    CameraMin;
            FVector2    Resolution;
            float       Time;
            uint32      LightID;
        };

        struct FBloomArgs
        {
            uint32   SourceID;
            uint32   bFirstPass;
            FVector2 SourceTexelSize;
            float    Threshold;
            float    Radius;
            float    Intensity;
            float    Pad0;
        };

        struct FCompositeArgs
        {
            uint32   SceneID;
            uint32   BloomID;
            FVector2 Resolution;
            float    BloomIntensity;
            float    Exposure;
            float    Vignette;
            float    Time;
            float    Danger;
            float    Hurt;
            float    Fade;
            float    Pad0;
        };

        static_assert(sizeof(FGroundArgs) == 40, "Slang mirror expects a packed 40 byte block.");
        static_assert(sizeof(FLightArgs) == 40, "Slang mirror expects a packed 40 byte block.");
        static_assert(sizeof(FAgentArgs) == 56, "Slang mirror expects a packed 56 byte block.");
        static_assert(sizeof(FQuadArgs) == 48, "Slang mirror expects a packed 48 byte block.");
        static_assert(sizeof(FBloomArgs) == 32, "Slang mirror expects a packed 32 byte block.");
        static_assert(sizeof(FCompositeArgs) == 48, "Slang mirror expects a packed 48 byte block.");

        void SubmitCompile(const FString& Source, const char* DebugName, TVector<uint32>& Out)
        {
            FShaderCompileOptions Options;
            Options.DebugName = DebugName;
            Options.bGenerateReflectionData = false;

            GShaderCompiler->CompilerShaderRaw(Source, Options,
                [&Out](FShaderHeader Header) { Out = Move(Header.Binaries); });
        }

        RHI::FShaderSource ShaderSource(const TVector<uint32>& Spirv, const char* EntryPoint)
        {
            return RHI::FShaderSource
            {
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()),
                                                     Spirv.size() * sizeof(uint32)),
                .EntryPoint = EntryPoint,
            };
        }

        RHI::FBlendDesc PremultipliedBlend()
        {
            RHI::FBlendDesc Blend;
            Blend.bBlendEnable   = true;
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            return Blend;
        }

        RHI::FBlendDesc AdditiveBlend()
        {
            RHI::FBlendDesc Blend;
            Blend.bBlendEnable   = true;
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::One;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::One;
            return Blend;
        }

        RHI::FPipelineH MakePipeline(const TVector<uint32>& Vertex, const char* VertexEntry,
                                     const TVector<uint32>& Pixel, const char* PixelEntry,
                                     EFormat Format, const RHI::FBlendDesc& Blend)
        {
            if (Vertex.empty() || Pixel.empty())
            {
                return {};
            }

            const RHI::FColorTarget ColorTarget { .Format = Format, .Blend = Blend };

            RHI::FRasterDesc Raster;
            Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

            return RHI::CreateGraphicsPipeline(ShaderSource(Vertex, VertexEntry), ShaderSource(Pixel, PixelEntry), Raster);
        }

        RHI::FRect FullRect(const FUIntVector2& Extent)
        {
            return RHI::FRect { 0, int32(Extent.x), 0, int32(Extent.y) };
        }

        FVector4 Fade(const FVector4& Color, float Alpha)
        {
            return { Color.x, Color.y, Color.z, Color.w * Alpha };
        }

        float Pulse(float Time, float Speed)
        {
            return 0.5f + 0.5f * Math::Sin(Time * Speed);
        }

        void PushQuad(TVector<FQuadInstance>& Out, const FBody& Body, const FVisual& Visual)
        {
            FQuadInstance& Instance = Out.emplace_back();
            Instance.Center       = Body.Position;
            Instance.HalfSize     = Body.HalfSize;
            Instance.Rotation     = Body.Rotation;
            Instance.Color        = Visual.Color;
            Instance.Accent       = Visual.Accent;
            Instance.CornerRadius = Visual.CornerRadius;
            Instance.Glow         = Visual.Glow;
            Instance.Kind         = uint32(Visual.Kind);
            Instance.Param3       = Visual.Lit;
        }

        struct FFixedText
        {
            char Text[64] = {};
            int32 Length = 0;

            void Append(const char* Value)
            {
                for (int32 Index = 0; Value[Index] != '\0' && Length < 62; ++Index)
                {
                    Text[Length++] = Value[Index];
                }
                Text[Length] = '\0';
            }
        };

        void PushDim(TVector<FQuadInstance>& Out, const FVector2& Center, float Strength)
        {
            FQuadInstance& Instance = Out.emplace_back();
            Instance.Center       = { Center.x, Center.y + kViewHeight * 0.5f };
            Instance.HalfSize     = { kViewWidth * 1.4f, kViewHeight * 0.85f };
            Instance.Color        = { 0.005f, 0.006f, 0.014f, Strength };
            Instance.Accent       = Instance.Color;
            Instance.CornerRadius = 0.0f;
            Instance.Glow         = 0.0f;
            Instance.Kind         = uint32(EQuadKind::Rect);
        }

        void PushPanel(TVector<FQuadInstance>& Out, const FVector2& Center, const FVector2& Half,
                       const FVector4& Border, float BodyOpacity)
        {
            FQuadInstance& Instance = Out.emplace_back();
            Instance.Center       = Center;
            Instance.HalfSize     = Half;
            Instance.Color        = Border;
            Instance.Accent       = { 0.020f, 0.026f, 0.052f, 1.0f };
            Instance.CornerRadius = 0.10f;
            Instance.Glow         = 0.35f;
            Instance.Kind         = uint32(EQuadKind::Panel);
            Instance.Param0       = 3.0f;
            Instance.Param1       = 2.0f;
            Instance.Param2       = BodyOpacity;
        }

        void PushIcon(TVector<FQuadInstance>& Out, const FVector2& Center, float Radius, EWeapon Weapon,
                      const FVector4& Color, float Glow)
        {
            FQuadInstance& Instance = Out.emplace_back();
            Instance.Center   = Center;
            Instance.HalfSize = { Radius, Radius };
            Instance.Rotation = Weapon == EWeapon::Blades ? 2.4f : 0.0f;
            Instance.Color    = Color;
            Instance.Accent   = Color;
            Instance.Kind     = uint32(WeaponIcon(Weapon));
            Instance.Glow     = Glow;
            Instance.Param0   = 0.16f;
        }

        // A segmented bar reads a value far faster than a plain fill does.
        void PushMeter(TVector<FQuadInstance>& Body, TVector<FQuadInstance>& Glow, const FVector2& Center,
                       float Width, float Height, float Fill, const FVector4& Color, int32 Segments)
        {
            FQuadInstance& Track = Body.emplace_back();
            Track.Center       = Center;
            Track.HalfSize     = { Width * 0.5f, Height };
            Track.Color        = { Color.x * 0.10f, Color.y * 0.10f, Color.z * 0.10f, 0.9f };
            Track.Accent       = Track.Color;
            Track.CornerRadius = 1.0f;
            Track.Glow         = 0.0f;
            Track.Kind         = uint32(EQuadKind::Rect);

            const float Clamped = Math::Clamp(Fill, 0.0f, 1.0f);
            const float Filled = Math::Max(Width * Clamped, 2.0f);

            FQuadInstance& Level = Glow.emplace_back();
            Level.Center       = { Center.x - Width * 0.5f + Filled * 0.5f, Center.y };
            Level.HalfSize     = { Filled * 0.5f, Height * 0.82f };
            Level.Color        = Color;
            Level.Accent       = Color;
            Level.CornerRadius = 1.0f;
            Level.Glow         = 0.30f;
            Level.Kind         = uint32(EQuadKind::Rect);

            for (int32 Tick = 1; Tick < Segments; ++Tick)
            {
                FQuadInstance& Notch = Body.emplace_back();
                Notch.Center       = { Center.x - Width * 0.5f + Width * float(Tick) / float(Segments), Center.y };
                Notch.HalfSize     = { 1.0f, Height };
                Notch.Color        = { 0.010f, 0.013f, 0.026f, 0.9f };
                Notch.Accent       = Notch.Color;
                Notch.CornerRadius = 0.0f;
                Notch.Glow         = 0.0f;
                Notch.Kind         = uint32(EQuadKind::Rect);
            }
        }

        void PushPips(TVector<FQuadInstance>& Body, TVector<FQuadInstance>& Glow, const FVector2& Start,
                      int32 Filled, int32 Total, const FVector4& Color)
        {
            for (int32 Index = 0; Index < Total; ++Index)
            {
                const bool bLit = Index < Filled;
                TVector<FQuadInstance>& Target = bLit ? Glow : Body;

                FQuadInstance& Pip = Target.emplace_back();
                Pip.Center       = { Start.x + float(Index) * 32.0f, Start.y };
                Pip.HalfSize     = { 11.0f, 5.0f };
                Pip.Color        = bLit ? Color : FVector4{ Color.x * 0.16f, Color.y * 0.16f, Color.z * 0.16f, 0.9f };
                Pip.Accent       = Pip.Color;
                Pip.CornerRadius = 1.0f;
                Pip.Glow         = bLit ? 0.35f : 0.0f;
                Pip.Kind         = uint32(EQuadKind::Rect);
            }
        }

        void PushBar(TVector<FQuadInstance>& Out, const FVector2& Center, float Width, float Height,
                     float Fill, const FVector4& Color)
        {
            FQuadInstance& Track = Out.emplace_back();
            Track.Center       = Center;
            Track.HalfSize     = { Width * 0.5f, Height };
            Track.Color        = { Color.x * 0.14f, Color.y * 0.14f, Color.z * 0.14f, 1.0f };
            Track.Accent       = Track.Color;
            Track.CornerRadius = 1.0f;
            Track.Glow         = 0.0f;

            const float Filled = Math::Max(Width * Math::Clamp(Fill, 0.0f, 1.0f), 1.0f);

            FQuadInstance& Level = Out.emplace_back();
            Level.Center       = { Center.x - Width * 0.5f + Filled * 0.5f, Center.y };
            Level.HalfSize     = { Filled * 0.5f, Height };
            Level.Color        = Color;
            Level.Accent       = Color;
            Level.CornerRadius = 1.0f;
            Level.Glow         = 0.7f;
        }
    }


    bool FRenderer::Initialize(EFormat InSwapchainFormat)
    {
        SwapchainFormat = InSwapchainFormat;

        AlphaQuads.reserve(8192);
        AdditiveQuads.reserve(8192);
        UiQuads.reserve(4096);
        UiGlowQuads.reserve(4096);
        Lights.reserve(kMaxLights);

        return CreatePipelines();
    }

    bool FRenderer::CreatePipelines()
    {
        const FString Prelude = FString(Shaders::kPrelude);
        const FString LightCommon = Prelude + Shaders::kLightCommon;
        const FString AgentCommon = Prelude + Shaders::kAgentCommon;
        const FString QuadCommon = Prelude + Shaders::kQuadCommon;

        TVector<uint32> FullscreenVS;
        TVector<uint32> GroundPS;
        TVector<uint32> LightVS;
        TVector<uint32> LightPS;
        TVector<uint32> AgentVS;
        TVector<uint32> AgentPS;
        TVector<uint32> QuadVS;
        TVector<uint32> QuadPS;
        TVector<uint32> QuadAdditivePS;
        TVector<uint32> DownsamplePS;
        TVector<uint32> UpsamplePS;
        TVector<uint32> CompositePS;

        SubmitCompile(Prelude + Shaders::kFullscreenVS, "Umbral.FullscreenVS", FullscreenVS);
        SubmitCompile(Prelude + Shaders::kGroundCommon + Shaders::kGroundPS, "Umbral.GroundPS", GroundPS);
        SubmitCompile(LightCommon + Shaders::kLightVS, "Umbral.LightVS", LightVS);
        SubmitCompile(LightCommon + Shaders::kLightPS, "Umbral.LightPS", LightPS);
        SubmitCompile(AgentCommon + Shaders::kAgentVS, "Umbral.AgentVS", AgentVS);
        SubmitCompile(AgentCommon + Shaders::kAgentPS, "Umbral.AgentPS", AgentPS);
        SubmitCompile(QuadCommon + Shaders::kQuadVS, "Umbral.QuadVS", QuadVS);
        SubmitCompile(QuadCommon + Shaders::kQuadPS, "Umbral.QuadPS", QuadPS);
        SubmitCompile(QuadCommon + Shaders::kQuadAdditivePS, "Umbral.QuadAdditivePS", QuadAdditivePS);
        SubmitCompile(Prelude + Shaders::kBloomCommon + Shaders::kDownsamplePS, "Umbral.DownsamplePS", DownsamplePS);
        SubmitCompile(Prelude + Shaders::kBloomCommon + Shaders::kUpsamplePS, "Umbral.UpsamplePS", UpsamplePS);
        SubmitCompile(Prelude + Shaders::kCompositeCommon + Shaders::kCompositePS, "Umbral.CompositePS", CompositePS);

        GShaderCompiler->Flush();

        GroundPipeline       = MakePipeline(FullscreenVS, "FullscreenVS", GroundPS, "GroundPS", kSceneFormat, {});
        LightPipeline        = MakePipeline(LightVS, "LightVS", LightPS, "LightPS", kSceneFormat, AdditiveBlend());
        AgentPipeline        = MakePipeline(AgentVS, "AgentVS", AgentPS, "AgentPS", kSceneFormat, PremultipliedBlend());
        QuadAlphaPipeline    = MakePipeline(QuadVS, "QuadVS", QuadPS, "QuadPS", kSceneFormat, PremultipliedBlend());
        QuadAdditivePipeline = MakePipeline(QuadVS, "QuadVS", QuadAdditivePS, "QuadAdditivePS", kSceneFormat, AdditiveBlend());
        DownsamplePipeline   = MakePipeline(FullscreenVS, "FullscreenVS", DownsamplePS, "DownsamplePS", kSceneFormat, {});
        UpsamplePipeline     = MakePipeline(FullscreenVS, "FullscreenVS", UpsamplePS, "UpsamplePS", kSceneFormat, AdditiveBlend());
        CompositePipeline    = MakePipeline(FullscreenVS, "FullscreenVS", CompositePS, "CompositePS", SwapchainFormat, {});

        const bool bReady = RHI::IsValid(GroundPipeline) && RHI::IsValid(LightPipeline)
                         && RHI::IsValid(AgentPipeline) && RHI::IsValid(QuadAlphaPipeline)
                         && RHI::IsValid(QuadAdditivePipeline) && RHI::IsValid(DownsamplePipeline)
                         && RHI::IsValid(UpsamplePipeline) && RHI::IsValid(CompositePipeline);

        if (!bReady)
        {
            LOG_ERROR("Umbral: one or more pipelines failed to build.");
        }
        return bReady;
    }

    void FRenderer::Shutdown()
    {
        RHI::WaitDeviceIdle();
        ReleaseTargets();

        for (RHI::FPipelineH Pipeline : { GroundPipeline, LightPipeline, AgentPipeline, QuadAlphaPipeline,
                                          QuadAdditivePipeline, DownsamplePipeline, UpsamplePipeline, CompositePipeline })
        {
            if (RHI::IsValid(Pipeline))
            {
                RHI::Retire(Pipeline);
            }
        }

    }

    void FRenderer::ReleaseTargets()
    {
        if (SceneTarget.IsValid())
        {
            RHI::Textures::Release(SceneTarget);
        }
        if (LightTarget.IsValid())
        {
            RHI::Textures::Release(LightTarget);
        }
        BloomChain.Shutdown();
    }

    void FRenderer::EnsureTargets(const FUIntVector2& Extent)
    {
        if (TargetExtent.x == Extent.x && TargetExtent.y == Extent.y)
        {
            return;
        }

        RHI::WaitDeviceIdle();
        ReleaseTargets();

        SceneTarget = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width = Extent.x, .Height = Extent.y, .Format = kSceneFormat,
            .bRenderTarget = true, .DebugName = "Umbral.Scene",
        });

        LightExtent = { Math::Max(1u, Extent.x >> 1u), Math::Max(1u, Extent.y >> 1u) };
        LightTarget = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width = LightExtent.x, .Height = LightExtent.y, .Format = kSceneFormat,
            .bRenderTarget = true, .DebugName = "Umbral.Lights",
        });

        BloomChain.Initialize(Extent, kBloomLevels, kSceneFormat, "Umbral.Bloom");

        TargetExtent = Extent;
    }

    void FRenderer::Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                           FGame& Game, float RealTime)
    {
        if (Extent.x == 0 || Extent.y == 0 || !SceneTarget.IsValid())
        {
            return;
        }

        BeatPulse = Math::Pow(1.0f - Math::Fract(RealTime / kBeatSeconds), 3.0f);

        const FRunState& Run = Game.GetRun();
        const FPlayerState& Player = Game.GetPlayer();

        // The view is a fixed slice of world units, letterboxed and then scaled to fill the window.
        const float Aspect = float(Extent.x) / float(Math::Max(Extent.y, 1u));
        ViewSize = { kViewHeight * Aspect, kViewHeight };
        UnitsToPixels = float(Extent.y) / kViewHeight;

        CameraMin =
        {
            Player.Position.x - ViewSize.x * 0.5f + Run.ShakeOffset.x,
            Player.Position.y - ViewSize.y * 0.5f + Run.ShakeOffset.y,
        };

        Gather(Game, RealTime);

        PlayerWorld = Player.Position;

        DrawLights(CL);
        DrawScene(CL, Extent, RealTime, Run);
        DrawBloom(CL);
        DrawComposite(CL, SwapImage, Extent, RealTime, Run, Player, Game.IsPaused());
    }

    void FRenderer::Gather(FGame& Game, float RealTime)
    {
        AlphaQuads.clear();
        AdditiveQuads.clear();
        UiQuads.clear();
        UiGlowQuads.clear();
        Lights.clear();

        ECS::FRegistry& Registry = Game.GetRegistry();
        const FPlayerState& Player = Game.GetPlayer();
        const FRunState& Run = Game.GetRun();
        const FWeaponState& Weapons = Game.GetWeapons();

        const FVector2 ViewMin { CameraMin.x - 200.0f, CameraMin.y - 200.0f };
        const FVector2 ViewMax { CameraMin.x + ViewSize.x + 200.0f, CameraMin.y + ViewSize.y + 200.0f };

        //~ The swarm is written straight into the transient ring, never into an intermediate vector.

        AgentCount = 0;
        AgentBuffer = 0;

        const int32 Capacity = Math::Min(kMaxDrawnAgents, Game.GetSwarm().Num());
        if (Capacity > 0)
        {
            const RHI::FTransientAlloc Block = RHI::AllocTransient(sizeof(FAgentInstance) * uint64(Capacity));
            if (Block.Cpu != nullptr)
            {
                AgentCount = Game.GetSwarm().GatherVisible(ViewMin, ViewMax, static_cast<FAgentInstance*>(Block.Cpu), Capacity);
                AgentBuffer = Block.Gpu;
            }
        }
        Game.GetStats().Drawn = AgentCount;

        //~ Player torch anchors the lighting.

        {
            FLightInstance& Torch = Lights.emplace_back();
            Torch.Center = Player.Position;
            Torch.Radius = 620.0f + Pulse(RealTime, 3.0f) * 30.0f;
            Torch.Energy = 1.5f * Player.Torch;
            Torch.Color  = { 0.55f, 0.72f, 1.05f, 1.0f };
        }

        for (auto [Entity, Body, Light] : Registry.View<FBody, FLight>().Each())
        {
            if (Lights.size() >= kMaxLights)
            {
                break;
            }
            if (Body.Position.x < ViewMin.x - Light.Radius || Body.Position.x > ViewMax.x + Light.Radius ||
                Body.Position.y < ViewMin.y - Light.Radius || Body.Position.y > ViewMax.y + Light.Radius)
            {
                continue;
            }

            FLightInstance& Instance = Lights.emplace_back();
            Instance.Center = Body.Position;
            Instance.Radius = Light.Radius;
            Instance.Energy = Light.Energy;
            Instance.Color  = Light.Color;
        }

        //~ World props, drawn after the agents so effects sit on top.

        for (auto [Entity, Body, Pyre, Visual] : Registry.View<FBody, FPyre, FVisual>().Each())
        {
            const float Alpha = Pyre.Age / Math::Max(Pyre.Duration, 0.001f);
            FVisual Flicker = Visual;
            Flicker.Color = { Visual.Color.x, Visual.Color.y, Visual.Color.z, 1.0f - Alpha * Alpha };
            PushQuad(AdditiveQuads, Body, Flicker);
        }

        for (auto [Entity, Body, Nova, Visual] : Registry.View<FBody, FNova, FVisual>().Each())
        {
            const float Alpha = Nova.Age / Math::Max(Nova.Duration, 0.001f);
            FQuadInstance& Ring = AdditiveQuads.emplace_back();
            Ring.Center   = Body.Position;
            Ring.HalfSize = Body.HalfSize;
            Ring.Color    = { Visual.Color.x, Visual.Color.y, Visual.Color.z, (1.0f - Alpha) * (1.0f - Alpha) };
            Ring.Accent   = Ring.Color;
            Ring.Kind     = uint32(EQuadKind::Ring);
            Ring.Glow     = 1.2f;
            Ring.Param0   = Math::Lerp(0.28f, 0.03f, Alpha);
        }

        for (auto [Entity, Arc] : Registry.View<FArc>().Each())
        {
            const float Alpha = Math::Clamp(Arc.Age / Math::Max(Arc.Life, 0.001f), 0.0f, 1.0f);
            const float Strength = 1.0f - Alpha;
            const FVector2 Span { Arc.To.x - Arc.From.x, Arc.To.y - Arc.From.y };
            const float Length = Math::Sqrt(Span.x * Span.x + Span.y * Span.y);
            if (Length < 1.0f)
            {
                continue;
            }

            const FVector2 Along { Span.x / Length, Span.y / Length };
            const FVector2 Side { -Along.y, Along.x };
            const int32 Segments = 5;

            FVector2 Previous = Arc.From;
            for (int32 Step = 1; Step <= Segments; ++Step)
            {
                const float T = float(Step) / float(Segments);
                const float Jitter = Step == Segments ? 0.0f
                    : Math::Sin(T * 18.0f + float(Entity.GetIndex()) * 2.7f + RealTime * 40.0f) * Length * 0.06f;

                const FVector2 Node { Arc.From.x + Span.x * T + Side.x * Jitter,
                                      Arc.From.y + Span.y * T + Side.y * Jitter };
                const FVector2 Leg { Node.x - Previous.x, Node.y - Previous.y };
                const float LegLength = Math::Sqrt(Leg.x * Leg.x + Leg.y * Leg.y);

                FQuadInstance& Bolt = AdditiveQuads.emplace_back();
                Bolt.Center   = { (Previous.x + Node.x) * 0.5f, (Previous.y + Node.y) * 0.5f };
                Bolt.HalfSize = { LegLength * 0.5f + 3.0f, 4.5f * Strength + 1.0f };
                Bolt.Rotation = Math::Atan2(Leg.y, Leg.x);
                Bolt.Color    = { 1.10f * Strength, 1.05f * Strength, 0.50f * Strength, 1.0f };
                Bolt.Accent   = Bolt.Color;
                Bolt.Kind     = uint32(EQuadKind::Ribbon);
                Bolt.Glow     = 0.9f * Strength;
                Bolt.Param0   = 1.0f;

                Previous = Node;
            }
        }

        for (auto [Entity, Body, Maw, Visual] : Registry.View<FBody, FMaw, FVisual>().Each())
        {
            const float Alpha = Maw.Age / Math::Max(Maw.Duration, 0.001f);
            const float Swell = Math::Sin(Alpha * 3.14159f);

            for (int32 Ring = 0; Ring < 3; ++Ring)
            {
                FQuadInstance& Vortex = AdditiveQuads.emplace_back();
                Vortex.Center   = Body.Position;
                Vortex.HalfSize = Body.HalfSize * (1.0f - float(Ring) * 0.24f);
                Vortex.Rotation = Maw.Spin * (Ring % 2 == 0 ? 1.0f : -1.0f);
                Vortex.Color    = { 0.95f * Swell, 0.30f * Swell, 1.55f * Swell, 1.0f };
                Vortex.Accent   = Vortex.Color;
                Vortex.Kind     = uint32(EQuadKind::Ring);
                Vortex.Glow     = 0.9f * Swell;
                Vortex.Param0   = 0.05f + float(Ring) * 0.03f;
            }
        }

        for (auto [Entity, Body, Particle, Visual] : Registry.View<FBody, FParticle, FVisual>().Each())
        {
            const float Speed = Math::Sqrt(Particle.Velocity.x * Particle.Velocity.x
                                         + Particle.Velocity.y * Particle.Velocity.y);
            const float Stretch = 1.0f + Math::Min(Speed / 340.0f, 2.6f);

            FQuadInstance& Spark = AdditiveQuads.emplace_back();
            Spark.Center   = Body.Position;
            Spark.HalfSize = { Body.HalfSize.x * Stretch, Body.HalfSize.y / Math::Sqrt(Stretch) };
            Spark.Rotation = Speed > 1.0f ? Math::Atan2(Particle.Velocity.y, Particle.Velocity.x) : 0.0f;
            Spark.Color    = Visual.Color;
            Spark.Accent   = Visual.Accent;
            Spark.Kind     = uint32(EQuadKind::Spark);
            Spark.Glow     = Visual.Glow;
        }

        for (auto [Entity, Body, Mote, Visual] : Registry.View<FBody, FSoulMote, FVisual>().Each())
        {
            PushQuad(AdditiveQuads, Body, Visual);
            AdditiveQuads.back().Param1 = float(Entity.GetIndex() % 64) * 0.4f;
        }

        for (auto [Entity, Body, Projectile, Visual] : Registry.View<FBody, FProjectile, FVisual>().Each())
        {
            PushQuad(AdditiveQuads, Body, Visual);
        }

        //~ Player and orbiting blades.

        {
            FQuadInstance& Core = AlphaQuads.emplace_back();
            Core.Center   = Player.Position;
            Core.HalfSize = { kPlayerRadius * 1.7f, kPlayerRadius * 1.7f };
            Core.Color    = { 0.55f + Player.HurtFlash * 1.6f, 1.05f, 1.60f, 1.0f };
            Core.Accent   = { 0.10f, 0.30f, 0.60f, 1.0f };
            Core.Kind     = uint32(EQuadKind::Sigil);
            Core.Glow     = 1.1f;

            if (Weapons.Level[int32(EWeapon::Gloom)] > 0)
            {
                const float Radius = 240.0f + float(Weapons.Level[int32(EWeapon::Gloom)]) * 26.0f;

                for (int32 Ring = 0; Ring < 2; ++Ring)
                {
                    FQuadInstance& Tide = AdditiveQuads.emplace_back();
                    Tide.Center   = Player.Position;
                    Tide.HalfSize = { Radius * (1.0f - float(Ring) * 0.12f), Radius * (1.0f - float(Ring) * 0.12f) };
                    Tide.Color    = { 0.10f, 0.30f, 0.22f, 1.0f };
                    Tide.Accent   = Tide.Color;
                    Tide.Kind     = uint32(EQuadKind::Ring);
                    Tide.Glow     = 0.30f;
                    Tide.Param0   = 0.018f + 0.010f * Pulse(RealTime * (Ring == 0 ? 1.0f : 1.7f), 2.2f);
                }
            }

            const int32 BladeLevel = Weapons.Level[int32(EWeapon::Blades)];
            const int32 Blades = BladeLevel > 0 ? 2 + BladeLevel : 0;
            const float Orbit = 140.0f + float(BladeLevel) * 12.0f;

            for (int32 Index = 0; Index < Blades; ++Index)
            {
                const float Angle = Player.BladePhase + 6.2831853f * float(Index) / float(Blades);

                FQuadInstance& Blade = AdditiveQuads.emplace_back();
                Blade.Center   = { Player.Position.x + Math::Cos(Angle) * Orbit,
                                   Player.Position.y + Math::Sin(Angle) * Orbit };
                Blade.HalfSize = { 42.0f, 42.0f };
                Blade.Rotation = Angle + 3.14159f;
                Blade.Color    = { 0.55f, 1.30f, 1.95f, 1.0f };
                Blade.Accent   = Blade.Color;
                Blade.Kind     = uint32(EQuadKind::Blade);
                Blade.Glow     = 0.85f;
            }
        }

        GatherHud(Game, RealTime);
    }

    void FRenderer::GatherHud(FGame& Game, float RealTime)
    {
        const FRunState& Run = Game.GetRun();
        const FPlayerState& Player = Game.GetPlayer();
        const FWeaponState& Weapons = Game.GetWeapons();

        // Authored in view units and offset by the camera, so the layout is resolution independent.
        const FVector2 Origin = CameraMin;
        const float Right = Origin.x + ViewSize.x;
        const FVector2 Center { Origin.x + ViewSize.x * 0.5f, Origin.y };

        const FVector4 Ink { 0.52f, 0.66f, 0.94f, 1.0f };
        const FVector4 Dim { 0.24f, 0.31f, 0.50f, 1.0f };
        const FVector4 Gold { 0.95f, 0.76f, 0.34f, 1.0f };
        const FVector4 Blood { 0.90f, 0.20f, 0.24f, 1.0f };
        const FVector4 Soul { 0.36f, 0.92f, 1.20f, 1.0f };

        char Buffer[48];

        //~ Vitals and soul charge, framed together on the left.

        PushPanel(UiQuads, { Origin.x + 250.0f, Origin.y + 74.0f }, { 210.0f, 56.0f }, Ink, 0.30f);

        Font::Emit(UiQuads, "VITALS", { Origin.x + 58.0f, Origin.y + 34.0f }, 3.5f, Dim, 0.0f);
        PushMeter(UiQuads, UiGlowQuads, { Origin.x + 250.0f, Origin.y + 64.0f }, 360.0f, 7.0f,
            Player.Health / kPlayerMaxHealth, Blood, 10);

        Font::FormatInt(Buffer, 48, int32(Math::Ceil(Player.Health)));
        Font::Emit(UiQuads, Buffer, { Origin.x + 430.0f, Origin.y + 34.0f }, 3.5f, Ink, 0.0f, Font::EAlign::Right);

        PushMeter(UiQuads, UiGlowQuads, { Origin.x + 250.0f, Origin.y + 96.0f }, 360.0f, 4.0f,
            Run.Souls / Math::Max(Run.SoulsNeeded, 1.0f), Soul, 0);

        Font::Emit(UiQuads, "LV", { Origin.x + 58.0f, Origin.y + 116.0f }, 4.0f, Dim, 0.0f);
        Font::FormatInt(Buffer, 48, Run.Level);
        Font::Emit(UiGlowQuads, Buffer, { Origin.x + 116.0f, Origin.y + 116.0f }, 4.5f, Soul, 0.25f);

        //~ Survival clock, centered.

        const int32 Seconds = int32(Run.Elapsed);
        Font::FormatInt(Buffer, 48, Seconds / 60);
        FFixedText Clock;
        Clock.Append(Buffer);
        Clock.Append(":");
        Font::FormatPadded(Buffer, 48, Seconds % 60, 2);
        Clock.Append(Buffer);

        Font::Emit(UiQuads, Clock.Text, { Center.x, Origin.y + 34.0f }, 7.0f, Ink, 0.12f, Font::EAlign::Center);

        //~ Kill tally, right.

        Font::Emit(UiQuads, "REAPED", { Right - 58.0f, Origin.y + 34.0f }, 3.5f, Dim, 0.0f, Font::EAlign::Right);
        Font::FormatInt(Buffer, 48, int32(Math::Min(Run.Kills, int64(99999999))));
        Font::Emit(UiQuads, Buffer, { Right - 58.0f, Origin.y + 68.0f }, 7.0f, Ink, 0.12f, Font::EAlign::Right);

        //~ Owned powers as icon plus pips along the bottom left.

        float SlotY = Origin.y + ViewSize.y - 150.0f;
        for (int32 Index = 0; Index < int32(EWeapon::Count); ++Index)
        {
            if (Weapons.Level[Index] <= 0)
            {
                continue;
            }

            const EWeapon Weapon = EWeapon(Index);
            const FVector4 Color = WeaponColor(Weapon);

            PushIcon(UiGlowQuads, { Origin.x + 78.0f, SlotY + 12.0f }, 20.0f, Weapon, Color, 0.55f);
            Font::Emit(UiQuads, WeaponName(Weapon), { Origin.x + 118.0f, SlotY }, 3.5f, Fade(Ink, 0.9f), 0.0f);
            PushPips(UiQuads, UiGlowQuads, { Origin.x + 122.0f, SlotY + 32.0f }, Weapons.Level[Index], 8, Color);

            SlotY += 60.0f;
        }

        //~ Banner after a pick, so the choice registers once the world comes back.

        if (Run.BannerTimer > 0.0f && Run.Phase == EPhase::Playing)
        {
            const float Alpha = Math::Min(Run.BannerTimer / 0.5f, 1.0f);
            Font::Emit(UiGlowQuads, "POWER CLAIMED", { Center.x, Origin.y + 250.0f }, 7.0f,
                Fade(Gold, Alpha), 0.5f, Font::EAlign::Center);
        }

        const float Beat = 0.55f + 0.45f * Pulse(RealTime, 3.0f);

        switch (Run.Phase)
        {
        case EPhase::Title:
        {
            PushDim(UiQuads, Center, 0.55f);

            PushPanel(UiQuads, { Center.x, Origin.y + 380.0f }, { 560.0f, 190.0f },
                { 0.62f, 0.30f, 1.00f, 1.0f }, 0.45f);

            Font::Emit(UiQuads, "UMBRAL", { Center.x, Origin.y + 300.0f }, 24.0f,
                { 0.66f, 0.32f, 1.05f, 1.0f }, 0.30f, Font::EAlign::Center);
            Font::Emit(UiQuads, "THE DARK REMEMBERS EVERY STEP YOU TAKE", { Center.x, Origin.y + 480.0f },
                3.8f, Dim, 0.0f, Font::EAlign::Center);

            Font::Emit(UiQuads, "PRESS SPACE TO DESCEND", { Center.x, Origin.y + 620.0f }, 6.5f,
                Fade(Ink, Beat), 0.12f, Font::EAlign::Center);

            PushPanel(UiQuads, { Center.x, Origin.y + 790.0f }, { 470.0f, 62.0f }, Dim, 0.28f);
            Font::Emit(UiQuads, "WASD MOVE", { Center.x, Origin.y + 760.0f }, 3.5f, Ink, 0.0f, Font::EAlign::Center);
            Font::Emit(UiQuads, "1 2 3 CHOOSE   P PAUSE   F3 STATS", { Center.x, Origin.y + 800.0f },
                3.5f, Dim, 0.0f, Font::EAlign::Center);
            break;
        }

        case EPhase::LevelUp:
        {
            const float Enter = Math::Clamp(Run.PhaseTimer / 0.32f, 0.0f, 1.0f);
            const float Ease = 1.0f - (1.0f - Enter) * (1.0f - Enter);

            PushDim(UiQuads, Center, 0.72f * Ease);

            Font::Emit(UiQuads, "LEVEL", { Center.x, Origin.y + 150.0f }, 4.5f, Dim, 0.0f, Font::EAlign::Center);
            Font::FormatInt(Buffer, 48, Run.Level);
            Font::Emit(UiGlowQuads, Buffer, { Center.x, Origin.y + 196.0f }, 13.0f,
                Fade(Gold, Ease), 0.45f, Font::EAlign::Center);
            Font::Emit(UiQuads, "THE VOID OFFERS", { Center.x, Origin.y + 330.0f }, 6.0f,
                Fade({ 0.72f, 0.38f, 1.10f, 1.0f }, Ease), 0.20f, Font::EAlign::Center);

            for (int32 Slot = 0; Slot < Run.ChoiceCount; ++Slot)
            {
                const float Stagger = Math::Clamp((Run.PhaseTimer - float(Slot) * 0.07f) / 0.30f, 0.0f, 1.0f);
                const float Slide = (1.0f - Stagger) * (1.0f - Stagger);
                if (Stagger <= 0.0f)
                {
                    continue;
                }

                const EWeapon Weapon = EWeapon(Run.Choices[Slot]);
                const int32 Current = Weapons.Level[int32(Weapon)];
                const bool bHovered = Run.Hovered == Slot;

                const FVector4 Base = WeaponColor(Weapon);
                const FVector4 Color = bHovered ? Fade(Base, 1.0f) : Base;
                const float Breathe = 1.0f + 0.02f * Math::Sin(RealTime * 3.4f + float(Slot) * 1.3f)
                                    + (bHovered ? 0.05f : 0.0f);

                const FCardRect Rect = LevelUpCardRect(Slot, ViewSize);
                const FVector2 CardCenter { Origin.x + Rect.Center.x,
                                            Origin.y + Rect.Center.y + Slide * 90.0f - (bHovered ? 16.0f : 0.0f) };
                const FVector2 CardHalf { Rect.Half.x * Breathe, Rect.Half.y * Breathe };

                if (bHovered)
                {
                    FQuadInstance& Halo = UiGlowQuads.emplace_back();
                    Halo.Center       = CardCenter;
                    Halo.HalfSize     = CardHalf;
                    Halo.Color        = { Base.x * 0.22f, Base.y * 0.22f, Base.z * 0.22f, Stagger };
                    Halo.Accent       = Halo.Color;
                    Halo.CornerRadius = 0.10f;
                    Halo.Glow         = 1.0f;
                    Halo.Kind         = uint32(EQuadKind::Rect);
                }

                PushPanel(UiQuads, CardCenter, CardHalf, Fade(Color, Stagger), bHovered ? 0.78f : 0.60f);

                PushIcon(UiGlowQuads, { CardCenter.x, CardCenter.y - kCardHeight * 0.28f },
                    bHovered ? 62.0f : 54.0f, Weapon, Fade(Color, Stagger), bHovered ? 1.3f : 0.9f);

                Font::Emit(UiQuads, WeaponName(Weapon), { CardCenter.x, CardCenter.y - 60.0f }, 5.5f,
                    Fade({ 0.80f, 0.90f, 1.10f, 1.0f }, Stagger), 0.15f, Font::EAlign::Center);

                Font::Emit(UiQuads, WeaponTagline(Weapon), { CardCenter.x, CardCenter.y - 8.0f }, 3.2f,
                    Fade(Dim, Stagger), 0.0f, Font::EAlign::Center);

                PushPips(UiQuads, UiGlowQuads, { CardCenter.x - 128.0f, CardCenter.y + 52.0f },
                    Current + 1, 8, Fade(Color, Stagger));

                if (Current <= 0)
                {
                    Font::Emit(UiGlowQuads, "NEW", { CardCenter.x, CardCenter.y + 104.0f }, 4.5f,
                        Fade(Gold, Stagger), 0.4f, Font::EAlign::Center);
                }
                else
                {
                    Font::FormatInt(Buffer, 48, Current + 1);
                    FFixedText Rank;
                    Rank.Append("RANK ");
                    Rank.Append(Buffer);
                    Font::Emit(UiQuads, Rank.Text, { CardCenter.x, CardCenter.y + 104.0f }, 4.0f,
                        Fade(Ink, Stagger), 0.0f, Font::EAlign::Center);
                }

                Font::Emit(UiQuads, WeaponUpgrade(Weapon, Current + 1), { CardCenter.x, CardCenter.y + 156.0f },
                    3.2f, Fade(Fade(Color, 1.4f), Stagger), 0.0f, Font::EAlign::Center);

                const FVector2 KeyCenter { CardCenter.x, CardCenter.y + kCardHeight * 0.36f };
                PushPanel(UiQuads, KeyCenter, { 42.0f, 34.0f }, Fade(Color, Stagger), 0.75f);
                Font::FormatInt(Buffer, 48, Slot + 1);
                Font::Emit(UiGlowQuads, Buffer, { KeyCenter.x, KeyCenter.y - 18.0f }, 5.5f,
                    Fade(Gold, Stagger), 0.35f, Font::EAlign::Center);
            }

            Font::Emit(UiQuads, "CLICK A CARD OR PRESS ITS NUMBER", { Center.x, Origin.y + 1010.0f }, 3.5f,
                Fade(Dim, Ease * Beat), 0.0f, Font::EAlign::Center);
            break;
        }

        case EPhase::Dead:
        {
            PushDim(UiQuads, Center, 0.75f);
            PushPanel(UiQuads, { Center.x, Origin.y + 480.0f }, { 520.0f, 300.0f }, Blood, 0.5f);

            Font::Emit(UiQuads, "CONSUMED", { Center.x, Origin.y + 300.0f }, 22.0f, Blood, 0.28f,
                Font::EAlign::Center);

            Font::Emit(UiQuads, "SOULS REAPED", { Center.x, Origin.y + 500.0f }, 3.8f, Dim, 0.0f,
                Font::EAlign::Center);
            Font::FormatInt(Buffer, 48, int32(Math::Min(Run.Kills, int64(99999999))));
            Font::Emit(UiGlowQuads, Buffer, { Center.x, Origin.y + 540.0f }, 11.0f, Fade(Ink, 0.9f), 0.30f,
                Font::EAlign::Center);

            Font::Emit(UiQuads, "SURVIVED", { Center.x, Origin.y + 640.0f }, 3.8f, Dim, 0.0f, Font::EAlign::Center);
            Font::FormatInt(Buffer, 48, Seconds / 60);
            FFixedText Lasted;
            Lasted.Append(Buffer);
            Lasted.Append(":");
            Font::FormatPadded(Buffer, 48, Seconds % 60, 2);
            Lasted.Append(Buffer);
            Font::Emit(UiQuads, Lasted.Text, { Center.x, Origin.y + 680.0f }, 7.0f, Ink, 0.12f, Font::EAlign::Center);

            Font::Emit(UiQuads, "PRESS SPACE TO RETURN", { Center.x, Origin.y + 820.0f }, 5.5f,
                Fade(Ink, Beat), 0.12f, Font::EAlign::Center);
            break;
        }

        default:
            break;
        }

        if (Game.IsPaused())
        {
            PushDim(UiQuads, Center, 0.6f);
            Font::Emit(UiQuads, "PAUSED", { Center.x, Origin.y + 460.0f }, 18.0f, Ink, 0.25f, Font::EAlign::Center);
            Font::Emit(UiQuads, "P RESUME   ESC QUIT", { Center.x, Origin.y + 600.0f }, 4.5f, Dim, 0.0f,
                Font::EAlign::Center);
        }

        if (Game.ShowsStats())
        {
            const FFrameStats& Stats = Game.GetStats();

            struct FStatRow { const char* Name; int32 Value; };
            const FStatRow Rows[6] =
            {
                { "MS",     int32(Stats.FrameMilliseconds * 100.0f) },
                { "WORST",  int32(Stats.WorstMilliseconds * 100.0f) },
                { "SWARM",  int32(Stats.SwarmMilliseconds * 100.0f) },
                { "AGENTS", Stats.Agents },
                { "DRAWN",  Stats.Drawn },
                { "PARTS",  Stats.Particles },
            };

            PushPanel(UiQuads, { Origin.x + 150.0f, Origin.y + 290.0f }, { 130.0f, 110.0f },
                { 0.30f, 0.90f, 0.45f, 1.0f }, 0.35f);

            float StatY = Origin.y + 200.0f;
            for (const FStatRow& Row : Rows)
            {
                Font::FormatInt(Buffer, 48, Row.Value);
                FFixedText Line;
                Line.Append(Row.Name);
                Line.Append(" ");
                Line.Append(Buffer);

                Font::Emit(UiQuads, Line.Text, { Origin.x + 40.0f, StatY }, 3.2f,
                    { 0.30f, 0.90f, 0.45f, 1.0f }, 0.0f);
                StatY += 30.0f;
            }
        }
    }

    void FRenderer::DrawLights(RHI::FCmdListH CL)
    {
        RHI::CmdBeginMarker(CL, "Umbral.Lights");
        RHI::Utils::BeginScreenPass(CL, { .Target = LightTarget.Texture, .Extent = LightExtent, });

        if (!Lights.empty())
        {
            const float ScaleX = 2.0f / Math::Max(ViewSize.x, 1.0f);
            const float ScaleY = 2.0f / Math::Max(ViewSize.y, 1.0f);

            const FLightArgs Args
            {
                .Lights    = RHI::CopyTransientArray(Lights.data(), Lights.size()).Address,
                .NdcScale  = { ScaleX, ScaleY },
                .NdcOffset = { -1.0f, -1.0f },
                .CameraMin = CameraMin,
                .Time      = 0.0f,
            };

            RHI::CmdSetPipeline(CL, LightPipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(Args), 6, uint32(Lights.size()), 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);
    }

    void FRenderer::DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, float RealTime, const FRunState& Run)
    {
        RHI::CmdBeginMarker(CL, "Umbral.Scene");
        RHI::Utils::BeginScreenPass(CL, { .Target = SceneTarget.Texture, .Extent = Extent, });

        const FGroundArgs Ground
        {
            .Resolution = { float(Extent.x), float(Extent.y) },
            .CameraMin  = CameraMin,
            .CameraSize = ViewSize,
            .Time       = RealTime,
            .Danger     = Run.Danger,
            .Beat       = BeatPulse,
        };
        RHI::Utils::DrawFullscreen(CL, GroundPipeline, RHI::CopyTransient(Ground));

        const float ScaleX = 2.0f / Math::Max(ViewSize.x, 1.0f);
        const float ScaleY = 2.0f / Math::Max(ViewSize.y, 1.0f);
        const FVector2 Resolution { float(Extent.x), float(Extent.y) };

        if (AgentCount > 0 && AgentBuffer != 0)
        {
            const FAgentArgs Args
            {
                .Agents    = AgentBuffer,
                .NdcScale  = { ScaleX, ScaleY },
                .NdcOffset = { -1.0f, -1.0f },
                .CameraMin = CameraMin,
                .Resolution = Resolution,
                .PlayerWorld = PlayerWorld,
                .Time      = RealTime,
                .LightID   = LightTarget.SampledSlot,
            };

            RHI::CmdSetPipeline(CL, AgentPipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(Args), 6, uint32(AgentCount), 0, 0);
        }

        FQuadArgs QuadArgs
        {
            .Instances = 0,
            .NdcScale  = { ScaleX, ScaleY },
            .NdcOffset = { -1.0f, -1.0f },
            .CameraMin = CameraMin,
            .Resolution = Resolution,
            .Time      = RealTime,
            .LightID   = LightTarget.SampledSlot,
        };

        if (!AlphaQuads.empty())
        {
            QuadArgs.Instances = RHI::CopyTransientArray(AlphaQuads.data(), AlphaQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAlphaPipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(QuadArgs), 6, uint32(AlphaQuads.size()), 0, 0);
        }

        if (!AdditiveQuads.empty())
        {
            QuadArgs.Instances = RHI::CopyTransientArray(AdditiveQuads.data(), AdditiveQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAdditivePipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(QuadArgs), 6, uint32(AdditiveQuads.size()), 0, 0);
        }

        if (!UiQuads.empty())
        {
            QuadArgs.Instances = RHI::CopyTransientArray(UiQuads.data(), UiQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAlphaPipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(QuadArgs), 6, uint32(UiQuads.size()), 0, 0);
        }

        if (!UiGlowQuads.empty())
        {
            QuadArgs.Instances = RHI::CopyTransientArray(UiGlowQuads.data(), UiGlowQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAdditivePipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(QuadArgs), 6, uint32(UiGlowQuads.size()), 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);
    }

    void FRenderer::DrawBloom(RHI::FCmdListH CL)
    {
        RHI::CmdBeginMarker(CL, "Umbral.Bloom");

        for (int32 Level = 0; Level < kBloomLevels; ++Level)
        {
            const uint32 SourceID = Level == 0 ? SceneTarget.SampledSlot : BloomChain.SampledSlot(Level - 1);
            const FVector2 SourceTexel = Level == 0
                ? FVector2{ 1.0f / float(TargetExtent.x), 1.0f / float(TargetExtent.y) }
                : BloomChain.TexelSize(Level - 1);

            const FBloomArgs Args
            {
                .SourceID        = SourceID,
                .bFirstPass      = Level == 0 ? 1u : 0u,
                .SourceTexelSize = SourceTexel,
                .Threshold       = 1.15f,
                .Radius          = 1.0f,
                .Intensity       = 1.0f,
            };

            RHI::Utils::BeginScreenPass(CL, { .Target = BloomChain.Texture(Level),
                .Extent = BloomChain.Extent(Level) });
            RHI::Utils::DrawFullscreen(CL, DownsamplePipeline, RHI::CopyTransient(Args));
            RHI::Utils::EndScreenPass(CL);
            RHI::Barriers::RasterToRead(CL);
        }

        for (int32 Level = kBloomLevels - 1; Level > 0; --Level)
        {
            const FBloomArgs Args
            {
                .SourceID        = BloomChain.SampledSlot(Level),
                .bFirstPass      = 0u,
                .SourceTexelSize = BloomChain.TexelSize(Level),
                .Threshold       = 0.0f,
                .Radius          = 1.30f,
                .Intensity       = 0.55f,
            };

            RHI::Utils::BeginScreenPass(CL, { .Target = BloomChain.Texture(Level - 1),
                .Extent = BloomChain.Extent(Level - 1),
                .LoadOp = RHI::ELoadOp::Load });
            RHI::Utils::DrawFullscreen(CL, UpsamplePipeline, RHI::CopyTransient(Args));
            RHI::Utils::EndScreenPass(CL);
            RHI::Barriers::RasterToRead(CL);
        }

        RHI::CmdEndMarker(CL);
    }

    void FRenderer::DrawComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                                  float RealTime, const FRunState& Run, const FPlayerState& Player, bool bPaused)
    {
        const FCompositeArgs Args
        {
            .SceneID        = SceneTarget.SampledSlot,
            .BloomID        = BloomChain.SampledSlot(0),
            .Resolution     = { float(Extent.x), float(Extent.y) },
            .BloomIntensity = 0.14f,
            .Exposure       = 1.00f,
            .Vignette       = 0.72f,
            .Time           = RealTime,
            .Danger         = Run.Danger,
            .Hurt           = Player.HurtFlash,
            .Fade           = bPaused ? 0.45f : 1.0f,
        };

        RHI::CmdBeginMarker(CL, "Umbral.Composite");
        RHI::Utils::BeginScreenPass(CL, { .Target = SwapImage, .Extent = Extent });
        RHI::Utils::DrawFullscreen(CL, CompositePipeline, RHI::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
    }
}
