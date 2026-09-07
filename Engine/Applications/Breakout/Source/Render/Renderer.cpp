#include "Renderer.h"

#include "Font.h"
#include "Shaders.h"
#include "Log/Log.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"

namespace Breakout
{
    namespace
    {
        constexpr EFormat kSceneFormat = EFormat::RGBA16_FLOAT;

        struct FBackgroundArgs
        {
            FVector2 Resolution;
            FVector2 FieldOrigin;
            FVector2 FieldSize;
            float    Time;
            float    Flash;
            float    Level;
            float    Beat;
            float    Danger;
            float    Progress;
            float    Fever;
            float    Pad0;
        };

        struct FQuadArgs
        {
            RHI::GPUPtr Instances;
            FVector2    NdcScale;
            FVector2    NdcOffset;
            FVector2    UnitsToPixels;
            float       Time;
            float       Pad0;
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
            float    Chroma;
            float    Vignette;
            float    Time;
            float    Flash;
            float    Fade;
            float    Danger;
            float    Fire;
            float    Beat;
            float    Fever;
            float    Zoom;
            float    Blind;
            float    Vault;
            float    Pad1;
            float    Pad2;
            FVector4 Waves[4];
        };

        static_assert(sizeof(FBackgroundArgs) == 56, "Slang mirror expects a packed 56 byte block.");
        static_assert(sizeof(FQuadArgs) == 40, "Slang mirror expects a packed 40 byte block.");
        static_assert(sizeof(FBloomArgs) == 32, "Slang mirror expects a packed 32 byte block.");
        static_assert(sizeof(FCompositeArgs) == 144, "Slang mirror expects a packed 144 byte block.");

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

        FVector4 Fade(const FVector4& Color, float Alpha)
        {
            return { Color.x, Color.y, Color.z, Color.w * Alpha };
        }

        void PushQuad(TVector<FQuadInstance>& Out, const FBody& Body, const FVisual& Visual, float Param0 = 0.0f,
                      float Param1 = 0.0f)
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
            Instance.Param0       = Param0;
            Instance.Param1       = Param1;
        }

        char PowerUpGlyph(EPowerUp Type)
        {
            switch (Type)
            {
            case EPowerUp::Widen:     return 'W';
            case EPowerUp::MultiBall: return 'M';
            case EPowerUp::SlowTime:  return 'S';
            case EPowerUp::ExtraLife: return '+';
            case EPowerUp::Laser:     return 'L';
            case EPowerUp::Fireball:  return 'F';
            case EPowerUp::Catch:     return 'C';
            case EPowerUp::Magnet:    return 'U';
            case EPowerUp::Shield:    return 'O';
            case EPowerUp::Bomb:      return 'B';
            case EPowerUp::BigBall:   return 'X';
            case EPowerUp::Wall:      return '#';
            case EPowerUp::Freeze:    return 'Z';
            case EPowerUp::Jackpot:   return 'J';
            case EPowerUp::Shrink:    return '-';
            case EPowerUp::SpeedUp:   return '>';
            case EPowerUp::Reverse:   return 'R';
            case EPowerUp::Blind:     return 'D';
            default:                  return 'V';
            }
        }

        void Append(char* Buffer, int32 Capacity, const char* Text)
        {
            int32 Written = Font::Length(Buffer);
            for (int32 i = 0; Text[i] != '\0' && Written < Capacity - 1; ++i)
            {
                Buffer[Written++] = Text[i];
            }
            Buffer[Written] = '\0';
        }

        void AppendInt(char* Buffer, int32 Capacity, int32 Value)
        {
            char Number[16];
            Font::FormatInt(Number, 16, Value);
            Append(Buffer, Capacity, Number);
        }

        FVector4 GradeColor(char Grade)
        {
            switch (Grade)
            {
            case 'S': return { 1.80f, 1.50f, 0.40f, 1.0f };
            case 'A': return { 0.50f, 1.60f, 0.80f, 1.0f };
            case 'B': return { 0.50f, 0.90f, 1.60f, 1.0f };
            default:  return { 1.20f, 0.50f, 0.60f, 1.0f };
            }
        }

        void PushPanel(TVector<FQuadInstance>& Out, const FVector2& Center, const FVector2& HalfSize,
                       const FVector4& Color, float Glow, float Corner = 0.12f)
        {
            FQuadInstance& Panel = Out.emplace_back();
            Panel.Center       = Center;
            Panel.HalfSize     = HalfSize;
            Panel.Color        = Color;
            Panel.Accent       = { Color.x * 0.35f, Color.y * 0.35f, Color.z * 0.45f, Color.w };
            Panel.CornerRadius = Corner;
            Panel.Glow         = Glow;
            Panel.Kind         = uint32(EQuadKind::Rect);
        }

        float Pulse(float Time, float Speed)
        {
            return 0.5f + 0.5f * Math::Sin(Time * Speed);
        }

        void PushBar(TVector<FQuadInstance>& Out, const FVector2& Center, float Width, float Fill, const FVector4& Color)
        {
            FQuadInstance& Track = Out.emplace_back();
            Track.Center       = Center;
            Track.HalfSize     = { Width * 0.5f, 4.0f };
            Track.Color        = { Color.x * 0.18f, Color.y * 0.18f, Color.z * 0.18f, 1.0f };
            Track.Accent       = Track.Color;
            Track.CornerRadius = 1.0f;
            Track.Glow         = 0.0f;

            const float Filled = Math::Max(Width * Math::Clamp(Fill, 0.0f, 1.0f), 1.0f);

            FQuadInstance& Level = Out.emplace_back();
            Level.Center       = { Center.x - Width * 0.5f + Filled * 0.5f, Center.y };
            Level.HalfSize     = { Filled * 0.5f, 4.0f };
            Level.Color        = Color;
            Level.Accent       = Color;
            Level.CornerRadius = 1.0f;
            Level.Glow         = 0.8f;
        }
    }


    bool FRenderer::Initialize(EFormat InSwapchainFormat)
    {
        SwapchainFormat = InSwapchainFormat;

        AlphaQuads.reserve(8192);
        AdditiveQuads.reserve(8192);

        return CreatePipelines();
    }

    bool FRenderer::CreatePipelines()
    {
        const FString Prelude = FString(Shaders::kPrelude);
        const FString QuadCommon = Prelude + Shaders::kQuadCommon;

        TVector<uint32> FullscreenVS;
        TVector<uint32> BackgroundPS;
        TVector<uint32> QuadVS;
        TVector<uint32> QuadPS;
        TVector<uint32> QuadAdditivePS;
        TVector<uint32> DownsamplePS;
        TVector<uint32> UpsamplePS;
        TVector<uint32> CompositePS;

        SubmitCompile(Prelude + Shaders::kFullscreenVS, "Breakout.FullscreenVS", FullscreenVS);
        SubmitCompile(Prelude + Shaders::kBackgroundCommon + Shaders::kBackgroundPS, "Breakout.BackgroundPS", BackgroundPS);
        SubmitCompile(QuadCommon + Shaders::kQuadVS, "Breakout.QuadVS", QuadVS);
        SubmitCompile(QuadCommon + Shaders::kQuadPS, "Breakout.QuadPS", QuadPS);
        SubmitCompile(QuadCommon + Shaders::kQuadAdditivePS, "Breakout.QuadAdditivePS", QuadAdditivePS);
        SubmitCompile(Prelude + Shaders::kBloomCommon + Shaders::kDownsamplePS, "Breakout.DownsamplePS", DownsamplePS);
        SubmitCompile(Prelude + Shaders::kBloomCommon + Shaders::kUpsamplePS, "Breakout.UpsamplePS", UpsamplePS);
        SubmitCompile(Prelude + Shaders::kCompositeCommon + Shaders::kCompositePS, "Breakout.CompositePS", CompositePS);

        GShaderCompiler->Flush();

        BackgroundPipeline   = MakePipeline(FullscreenVS, "FullscreenVS", BackgroundPS, "BackgroundPS", kSceneFormat, {});
        QuadAlphaPipeline    = MakePipeline(QuadVS, "QuadVS", QuadPS, "QuadPS", kSceneFormat, PremultipliedBlend());
        QuadAdditivePipeline = MakePipeline(QuadVS, "QuadVS", QuadAdditivePS, "QuadAdditivePS", kSceneFormat, AdditiveBlend());
        DownsamplePipeline   = MakePipeline(FullscreenVS, "FullscreenVS", DownsamplePS, "DownsamplePS", kSceneFormat, {});
        UpsamplePipeline     = MakePipeline(FullscreenVS, "FullscreenVS", UpsamplePS, "UpsamplePS", kSceneFormat, AdditiveBlend());
        CompositePipeline    = MakePipeline(FullscreenVS, "FullscreenVS", CompositePS, "CompositePS", SwapchainFormat, {});

        const bool bReady = RHI::IsValid(BackgroundPipeline) && RHI::IsValid(QuadAlphaPipeline)
                         && RHI::IsValid(QuadAdditivePipeline) && RHI::IsValid(DownsamplePipeline)
                         && RHI::IsValid(UpsamplePipeline) && RHI::IsValid(CompositePipeline);

        if (!bReady)
        {
            LOG_ERROR("Breakout: one or more pipelines failed to build.");
        }
        return bReady;
    }

    void FRenderer::Shutdown()
    {
        RHI::WaitDeviceIdle();
        ReleaseTargets();

        for (RHI::FPipelineH Pipeline : { BackgroundPipeline, QuadAlphaPipeline, QuadAdditivePipeline,
                                          DownsamplePipeline, UpsamplePipeline, CompositePipeline })
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
            .Width  = Extent.x,
            .Height = Extent.y,
            .Format = kSceneFormat,
            .bRenderTarget = true,
            .DebugName = "Breakout.Scene",
        });

        BloomChain.Initialize(Extent, kBloomLevels, kSceneFormat, "Breakout.Bloom");

        TargetExtent = Extent;
    }

    void FRenderer::Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                           FGame& Game, float RealTime)
    {
        if (Extent.x == 0 || Extent.y == 0)
        {
            return;
        }

        if (!SceneTarget.IsValid())
        {
            return;
        }

        const float Width = float(Extent.x);
        const float Height = float(Extent.y);
        const float FieldAspect = kFieldWidth / kFieldHeight;

        FieldSizePixels = Width / Height > FieldAspect
            ? FVector2 { Height * FieldAspect, Height }
            : FVector2 { Width, Width / FieldAspect };

        UnitsToPixels = FieldSizePixels.x / kFieldWidth;

        const FCameraShake& Shake = Game.GetShake();
        FieldOriginPixels =
        {
            (Width - FieldSizePixels.x) * 0.5f + Shake.Offset.x * UnitsToPixels,
            (Height - FieldSizePixels.y) * 0.5f + Shake.Offset.y * UnitsToPixels,
        };

        BeatPulse = Math::Pow(1.0f - Math::Fract(RealTime / kBeatSeconds), 3.0f);

        GatherQuads(Game, RealTime);
        GatherWarps(Game);

        const FGameState& State = Game.GetState();

        DrawScene(CL, Extent, RealTime, State);
        DrawBloom(CL);
        DrawComposite(CL, SwapImage, Extent, RealTime, State, Shake, Game.IsPaused());
    }

    void FRenderer::GatherWarps(FGame& Game)
    {
        for (FVector4& Warp : Warps)
        {
            Warp = { 0.0f, 0.0f, 0.0f, 0.0f };
        }

        int32 Count = 0;
        float Weakest = 1.0f;
        int32 WeakestSlot = 0;

        for (auto [Entity, Body, Wave] : Game.GetRegistry().View<FBody, FShockwave>().Each())
        {
            if (Wave.Warp <= 0.0f)
            {
                continue;
            }

            const float Alpha = Math::Clamp(Wave.Age / Math::Max(Wave.Duration, 0.001f), 0.0f, 1.0f);
            const float Strength = Wave.Warp * (1.0f - Alpha) * (1.0f - Alpha);
            if (Strength <= 0.01f)
            {
                continue;
            }

            const FVector2 Pixels = FieldOriginPixels + Body.Position * UnitsToPixels;
            const float RadiusPixels = Wave.MaxRadius * UnitsToPixels * (1.0f - (1.0f - Alpha) * (1.0f - Alpha));

            const FVector4 Entry
            {
                Pixels.x / Math::Max(float(TargetExtent.x), 1.0f),
                Pixels.y / Math::Max(float(TargetExtent.y), 1.0f),
                RadiusPixels / Math::Max(float(TargetExtent.y), 1.0f),
                Strength,
            };

            if (Count < 4)
            {
                Warps[Count] = Entry;
                if (Strength < Weakest)
                {
                    Weakest = Strength;
                    WeakestSlot = Count;
                }
                ++Count;
            }
            else if (Strength > Weakest)
            {
                Warps[WeakestSlot] = Entry;

                Weakest = 1.0f;
                for (int32 Slot = 0; Slot < 4; ++Slot)
                {
                    if (Warps[Slot].w < Weakest)
                    {
                        Weakest = Warps[Slot].w;
                        WeakestSlot = Slot;
                    }
                }
            }
        }
    }

    void FRenderer::GatherQuads(FGame& Game, float RealTime)
    {
        AlphaQuads.clear();
        AdditiveQuads.clear();

        ECS::FRegistry& Registry = Game.GetRegistry();

        const FGameState& State = Game.GetState();

        for (auto [Entity, Body, Brick, Visual] : Registry.View<FBody, FBrick, FVisual>().Each())
        {
            const float Damage = Brick.MaxHealth > 100
                ? 0.0f
                : 1.0f - float(Brick.Health) / float(Math::Max(1, Brick.MaxHealth));

            PushQuad(AlphaQuads, Body, Visual, Damage, float(Brick.Row * 13 + Brick.Column * 7));
            AlphaQuads.back().Param2 = float(uint32(Brick.Kind));

            if (Brick.Kind == EBrickKind::Gravity)
            {
                FQuadInstance& Halo = AdditiveQuads.emplace_back();
                Halo.Center   = Body.Position;
                Halo.HalfSize = { 300.0f + Pulse(RealTime, 4.0f) * 30.0f, 300.0f + Pulse(RealTime, 4.0f) * 30.0f };
                Halo.Color    = { 0.35f, 0.15f, 0.70f, 0.35f };
                Halo.Accent   = Halo.Color;
                Halo.Kind     = uint32(EQuadKind::Ring);
                Halo.Glow     = 0.3f;
                Halo.Param0   = 0.02f;
            }
        }

        for (auto [Entity, Body, Boss, Visual] : Registry.View<FBody, FBoss, FVisual>().Each())
        {
            PushQuad(AlphaQuads, Body, Visual, 1.0f - Boss.Health / Math::Max(Boss.MaxHealth, 1.0f),
                float(Entity.GetIndex()));

            EBrickKind Look = EBrickKind::Mystery;
            if (Boss.Kind == EBossKind::Architect)
            {
                Look = Boss.bArmored ? EBrickKind::Steel : EBrickKind::Gravity;
            }
            else if (Boss.Kind == EBossKind::Hydra)
            {
                Look = EBrickKind::Regen;
            }
            AlphaQuads.back().Param2 = float(uint32(Look));

            PushBar(AdditiveQuads, { Body.Position.x, Body.Position.y + Body.HalfSize.y + 26.0f },
                Body.HalfSize.x * 1.7f, Boss.Health / Math::Max(Boss.MaxHealth, 1.0f),
                Boss.bArmored ? FVector4{ 0.70f, 0.80f, 1.00f, 1.0f } : FVector4{ 1.40f, 0.30f, 0.90f, 1.0f });
        }

        for (auto [Entity, Body, Drone, Visual] : Registry.View<FBody, FDrone, FVisual>().Each())
        {
            PushQuad(AlphaQuads, Body, Visual, float(uint32(Drone.Kind)));

            FBody Core = Body;
            Core.HalfSize = Body.HalfSize * 0.35f;
            FVisual CoreVisual;
            CoreVisual.Color = { 2.0f, 1.8f, 1.6f, 1.0f };
            CoreVisual.Accent = CoreVisual.Color;
            CoreVisual.Kind = EQuadKind::Disc;
            CoreVisual.Glow = 1.2f + Drone.Flash;
            PushQuad(AdditiveQuads, Core, CoreVisual);
        }

        if (State.WallTimer > 0.0f)
        {
            const float Fade = Math::Min(1.0f, State.WallTimer * (State.WallTimer < 2.0f ? 0.5f + 0.5f * Pulse(RealTime, 14.0f) : 1.0f));
            FQuadInstance& Wall = AdditiveQuads.emplace_back();
            Wall.Center       = { kFieldWidth * 0.5f, kWallY };
            Wall.HalfSize     = { kFieldWidth * 0.5f, 5.0f + Pulse(RealTime, 6.0f) * 2.0f };
            Wall.Color        = { 0.25f * Fade, 1.10f * Fade, 0.70f * Fade, 1.0f };
            Wall.Accent       = Wall.Color;
            Wall.CornerRadius = 1.0f;
            Wall.Kind         = uint32(EQuadKind::Rect);
            Wall.Glow         = 1.0f;
        }

        for (auto [Entity, Body, Visual, Bolt] : Registry.View<FBody, FVisual, FLaserBolt>().Each())
        {
            FVisual Fading = Visual;
            Fading.Glow = Visual.Glow * Math::Min(1.0f, Bolt.Life * 3.0f);
            PushQuad(AdditiveQuads, Body, Fading);
        }

        for (auto [Entity, Body, Drop, Visual] : Registry.View<FBody, FPowerUpDrop, FVisual>().Each())
        {
            PushQuad(AlphaQuads, Body, Visual);

            const char Label[2] = { PowerUpGlyph(Drop.Type), '\0' };
            const FVector4 Ink { 1.20f, 1.30f, 1.55f, 1.0f };
            Font::Emit(AdditiveQuads, Label, { Body.Position.x, Body.Position.y - 10.5f }, 3.0f, Ink, 0.5f,
                Font::EAlign::Center);
        }

        for (auto [Entity, Body, Paddle, Visual] : Registry.View<FBody, FPaddle, FVisual>().Each())
        {
            FVisual Lit = Visual;
            Lit.Glow = Visual.Glow + Paddle.HitFlash * 1.2f;
            PushQuad(AlphaQuads, Body, Lit);

            ShieldGlow = Paddle.PlayerIndex == 0 ? Paddle.ShieldCharge : Math::Max(ShieldGlow, Paddle.ShieldCharge);

            if (Paddle.SmashArm > 0.0f || Paddle.SmashFlash > 0.0f)
            {
                const float Strength = Math::Max(Paddle.SmashArm / kSmashArmWindow, Paddle.SmashFlash);
                FQuadInstance& Arc = AdditiveQuads.emplace_back();
                Arc.Center   = { Body.Position.x, Body.Position.y };
                Arc.HalfSize = { Body.HalfSize.x + 30.0f + Paddle.SmashFlash * 120.0f, Body.HalfSize.x + 30.0f + Paddle.SmashFlash * 120.0f };
                Arc.Color    = { 1.80f * Strength, 1.50f * Strength, 0.90f * Strength, 1.0f };
                Arc.Accent   = Arc.Color;
                Arc.Kind     = uint32(EQuadKind::Ring);
                Arc.Glow     = 0.8f * Strength;
                Arc.Param0   = 0.08f;
            }

            if (Paddle.ShieldCharge > 0.02f || Paddle.ShieldFlash > 0.0f)
            {
                const bool bReady = Paddle.ShieldCharge >= 1.0f;
                const float Strength = Math::Max(Paddle.ShieldCharge, Paddle.ShieldFlash);

                FQuadInstance& Shield = AdditiveQuads.emplace_back();
                Shield.Center       = { kFieldWidth * 0.5f, kPaddleY + 44.0f };
                Shield.HalfSize     = { kFieldWidth * 0.5f * Paddle.ShieldCharge, 4.0f + Paddle.ShieldFlash * 22.0f };
                Shield.Color        = { 0.35f * Strength, 1.25f * Strength, 1.70f * Strength, 1.0f };
                Shield.Accent       = Shield.Color;
                Shield.CornerRadius = 1.0f;
                Shield.Kind         = uint32(EQuadKind::Rect);
                Shield.Glow         = (bReady ? 0.9f : 0.35f) + Paddle.ShieldFlash * 1.6f;
            }

            FBody Core = Body;
            Core.HalfSize = { Body.HalfSize.x * 0.82f, 3.0f };

            FVisual CoreVisual;
            CoreVisual.Color = { 0.55f + Paddle.HitFlash * 2.2f, 1.45f + Paddle.HitFlash * 2.2f, 2.00f, 1.0f };
            CoreVisual.Accent = CoreVisual.Color;
            CoreVisual.Kind = EQuadKind::Rect;
            CoreVisual.CornerRadius = 1.0f;
            CoreVisual.Glow = 0.7f + Paddle.HitFlash * 0.6f;
            PushQuad(AdditiveQuads, Core, CoreVisual);
        }

        for (auto [Entity, Body, Ball, Visual] : Registry.View<FBody, FBall, FVisual>().Each())
        {
            const int32 Nodes = Math::Min(int32(Ball.TrailCount), kBallTrailNodes);
            FVector2 Previous = Body.Position;

            for (int32 Node = 0; Node < Nodes; ++Node)
            {
                const FVector2 Next = Ball.Trail[Node];
                const FVector2 Segment = Next - Previous;
                const float Length = Math::Sqrt(Segment.x * Segment.x + Segment.y * Segment.y);
                Previous = Next;

                if (Length < 0.5f)
                {
                    continue;
                }

                const float Falloff = 1.0f - float(Node) / float(kBallTrailNodes);
                const float Taper = Falloff * Falloff;

                FQuadInstance& Ribbon = AdditiveQuads.emplace_back();
                Ribbon.Center   = Next - Segment * 0.5f;
                Ribbon.HalfSize = { Length * 0.5f + kBallRadius * 0.55f * Taper, kBallRadius * 0.92f * Taper };
                Ribbon.Rotation = Math::Atan2(Segment.y, Segment.x);
                Ribbon.Color    = { Visual.Color.x * 0.55f * Taper, Visual.Color.y * 0.55f * Taper,
                                    Visual.Color.z * 0.55f * Taper, 1.0f };
                Ribbon.Accent   = { Visual.Accent.x * 0.45f * Taper, Visual.Accent.y * 0.45f * Taper,
                                    Visual.Accent.z * 0.45f * Taper, 1.0f };
                Ribbon.Kind     = uint32(EQuadKind::Ribbon);
                Ribbon.Glow     = 0.45f * Taper;
                Ribbon.Param0   = Taper;
            }

            PushQuad(AlphaQuads, Body, Visual);

            FBody Core = Body;
            Core.HalfSize = Body.HalfSize * 0.55f;

            FVisual CoreVisual;
            CoreVisual.Color = { 1.55f, 1.85f, 2.30f, 1.0f };
            CoreVisual.Accent = CoreVisual.Color;
            CoreVisual.Kind = EQuadKind::Disc;
            CoreVisual.Glow = 1.1f;
            PushQuad(AdditiveQuads, Core, CoreVisual);
        }

        for (auto [Entity, Body, Particle, Visual] : Registry.View<FBody, FParticle, FVisual>().Each())
        {
            PushQuad(AdditiveQuads, Body, Visual);
        }

        for (auto [Entity, Body, Wave] : Registry.View<FBody, FShockwave>().Each())
        {
            const float Alpha = Math::Clamp(Wave.Age / Math::Max(Wave.Duration, 0.001f), 0.0f, 1.0f);
            const float Radius = Wave.MaxRadius * (1.0f - (1.0f - Alpha) * (1.0f - Alpha));
            const float Strength = (1.0f - Alpha) * (1.0f - Alpha);

            FQuadInstance& Instance = AdditiveQuads.emplace_back();
            Instance.Center   = Body.Position;
            Instance.HalfSize = { Radius, Radius };
            Instance.Color    = { Wave.Color.x * 1.35f, Wave.Color.y * 1.35f, Wave.Color.z * 1.35f, Strength };
            Instance.Accent   = Instance.Color;
            Instance.Kind     = uint32(EQuadKind::Ring);
            Instance.Glow     = 0.55f * Strength;
            Instance.Param0   = Math::Lerp(0.20f, 0.03f, Alpha);
        }

        for (auto [Entity, Pop] : Registry.View<FScorePop>().Each())
        {
            const float Alpha = Math::Clamp(Pop.Age / Math::Max(Pop.Duration, 0.001f), 0.0f, 1.0f);
            const float Strength = 1.0f - Alpha * Alpha;

            const FVector4 Ink { Pop.Color.x * 1.05f, Pop.Color.y * 1.05f, Pop.Color.z * 1.05f, Strength };
            float LineY = Pop.Position.y;

            if (Pop.Label[0] != '\0')
            {
                const float Bounce = Pop.Age < 0.15f ? 1.0f + (0.15f - Pop.Age) * 6.0f : 1.0f;
                Font::Emit(AdditiveQuads, Pop.Label, { Pop.Position.x, LineY }, Pop.Size * Bounce, Ink, 0.9f, Font::EAlign::Center);
                LineY += Pop.Size * 9.0f;
            }

            if (Pop.Value > 0)
            {
                char Buffer[16];
                Font::FormatInt(Buffer, 16, Pop.Value);
                Font::Emit(AdditiveQuads, Buffer, { Pop.Position.x, LineY }, 3.5f, Ink, 0.6f, Font::EAlign::Center);
            }
        }

        GatherHud(Game, RealTime);

        FFrameStats& Stats = Game.GetStats();
        Stats.AlphaQuads = int32(AlphaQuads.size());
        Stats.AdditiveQuads = int32(AdditiveQuads.size());
    }

    void FRenderer::GatherHud(FGame& Game, float RealTime)
    {
        const FGameState& State = Game.GetState();

        const FVector4 Label { 0.42f, 0.58f, 0.95f, 1.0f };
        const FVector4 Bright { 0.80f, 1.15f, 1.65f, 1.0f };
        const FVector4 Warm { 1.45f, 0.75f, 0.30f, 1.0f };
        const FVector4 Neon { 1.15f, 0.32f, 1.25f, 1.0f };
        const FVector4 Gold { 1.60f, 1.30f, 0.35f, 1.0f };

        char Buffer[64];

        Font::Emit(AlphaQuads, "SCORE", { 58.0f, 34.0f }, 5.0f, Label, 0.25f);
        Font::FormatPadded(Buffer, 64, State.DisplayScore, 7);
        Font::Emit(AdditiveQuads, Buffer, { 58.0f, 74.0f }, 8.0f, Bright, 0.75f);

        Font::Emit(AlphaQuads, "STAGE", { kFieldWidth * 0.5f, 34.0f }, 5.0f, Label, 0.25f, Font::EAlign::Center);
        Font::FormatInt(Buffer, 64, State.Level);
        Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 74.0f }, 8.0f, Bright, 0.75f, Font::EAlign::Center);

        const bool bInRun = State.Phase != EPhase::Title && State.Phase != EPhase::GameOver;
        const char* LevelName = Game.GetLevelName();
        if (bInRun && LevelName != nullptr && (State.Phase == EPhase::Serve || State.LevelTime < 3.0f) && State.BossIntro <= 0.0f)
        {
            Font::Emit(AlphaQuads, LevelName, { kFieldWidth * 0.5f, 118.0f }, 3.5f, Fade(Label, 1.2f), 0.2f, Font::EAlign::Center);
        }

        Font::Emit(AlphaQuads, "LIVES", { kFieldWidth - 58.0f, 34.0f }, 5.0f, Label, 0.25f, Font::EAlign::Right);

        for (int32 Life = 0; Life < State.Lives; ++Life)
        {
            FQuadInstance& Instance = AdditiveQuads.emplace_back();
            Instance.Center   = { kFieldWidth - 72.0f - Life * 44.0f, 96.0f };
            Instance.HalfSize = { 13.0f, 13.0f };
            Instance.Color    = { 0.60f, 1.20f, 1.70f, 1.0f };
            Instance.Accent   = Instance.Color;
            Instance.Kind     = uint32(EQuadKind::Disc);
            Instance.Glow     = 0.7f;
        }

        if (bInRun)
        {
            Font::Emit(AlphaQuads, GameModeName(State.Mode), { kFieldWidth - 58.0f, 130.0f }, 3.5f, Fade(Label, 1.1f), 0.2f, Font::EAlign::Right);
        }

        if (Game.IsMuted())
        {
            Font::Emit(AlphaQuads, "MUTED", { kFieldWidth - 58.0f, 162.0f }, 3.5f, Fade(Warm, 0.8f), 0.2f, Font::EAlign::Right);
        }

        const ECS::FEntity PaddleEntity = [&Game]() -> ECS::FEntity
        {
            for (const ECS::FEntity Entity : Game.GetRegistry().View<FPaddle>())
            {
                return Entity;
            }
            return ECS::NullEntity;
        }();

        if (!PaddleEntity.IsNull())
        {
            const FPaddle& Paddle = Game.GetRegistry().Get<FPaddle>(PaddleEntity);

            float FireTimer = 0.0f;
            float BigTimer = 0.0f;
            for (auto [BallEntity, Ball] : Game.GetRegistry().View<FBall>().Each())
            {
                FireTimer = Math::Max(FireTimer, Ball.FireTimer);
                BigTimer = Math::Max(BigTimer, Ball.BigTimer);
            }

            struct FBadge
            {
                const char* Name;
                float       Remaining;
                float       Total;
                FVector4    Color;
            };

            const FBadge Badges[13] =
            {
                { "WIDE",    Paddle.WidenTimer,   15.0f, { 0.30f, 1.05f, 1.30f, 1.0f } },
                { "LASER",   Paddle.LaserTimer,   16.0f, { 1.30f, 0.30f, 0.44f, 1.0f } },
                { "CATCH",   Paddle.CatchTimer,   14.0f, { 0.70f, 0.76f, 1.30f, 1.0f } },
                { "FIRE",    FireTimer,            9.0f, { 1.30f, 0.62f, 0.12f, 1.0f } },
                { "BIG",     BigTimer,            11.0f, { 1.10f, 1.20f, 1.40f, 1.0f } },
                { "SLOW",    State.SlowTimer,      7.0f, { 1.30f, 1.02f, 0.28f, 1.0f } },
                { "MAGNET",  Paddle.MagnetTimer,  14.0f, { 1.00f, 0.55f, 1.30f, 1.0f } },
                { "WALL",    State.WallTimer,     10.0f, { 0.30f, 1.20f, 0.80f, 1.0f } },
                { "FREEZE",  State.FreezeTimer,   13.0f, { 0.70f, 1.00f, 1.50f, 1.0f } },
                { "JACKPOT", State.JackpotTimer,  10.0f, { 1.50f, 1.30f, 0.30f, 1.0f } },
                { "SHRNK",   Paddle.ShrinkTimer,   9.0f, { 1.20f, 0.16f, 0.26f, 1.0f } },
                { "REVERSE", Paddle.ReverseTimer,  6.0f, { 1.20f, 0.20f, 0.80f, 1.0f } },
                { "BLIND",   State.BlindTimer,     5.5f, { 0.80f, 0.20f, 0.60f, 1.0f } },
            };

            float BadgeY = 1006.0f;
            for (const FBadge& Badge : Badges)
            {
                if (Badge.Remaining <= 0.0f)
                {
                    continue;
                }

                const float Blink = Badge.Remaining < 3.0f ? 0.45f + 0.55f * Pulse(RealTime, 12.0f) : 1.0f;
                Font::Emit(AdditiveQuads, Badge.Name, { 58.0f, BadgeY }, 4.0f, Fade(Badge.Color, Blink), 0.5f);
                PushBar(AlphaQuads, { 254.0f, BadgeY + 14.0f }, 150.0f, Badge.Remaining / Badge.Total, Badge.Color);
                BadgeY -= 42.0f;
            }
        }

        if (bInRun)
        {
            float PerkY = 966.0f;
            for (int32 Index = 0; Index < int32(EPerk::Count); ++Index)
            {
                const EPerk Perk = EPerk(Index);
                const int32 Stacks = State.Perks.Count(Perk);
                if (Stacks <= 0 || Perk == EPerk::Lifeline)
                {
                    continue;
                }

                Buffer[0] = '\0';
                Append(Buffer, 64, PerkInfo(Perk).Name);
                if (Stacks > 1)
                {
                    Append(Buffer, 64, " x");
                    AppendInt(Buffer, 64, Stacks);
                }
                Font::Emit(AlphaQuads, Buffer, { kFieldWidth - 58.0f, PerkY }, 3.0f, Fade(Gold, 0.9f), 0.25f, Font::EAlign::Right);
                PerkY -= 28.0f;
            }
        }

        const float FeverFill = State.FeverMeter;
        if (FeverFill > 0.01f || State.IsFever())
        {
            const bool bBurning = State.IsFever();
            const float Wobble = bBurning ? Pulse(RealTime, 14.0f) : 0.0f;
            const FVector4 Meter = bBurning
                ? FVector4{ 2.20f + Wobble, 0.70f + Wobble * 0.5f, 2.20f, 1.0f }
                : FVector4{ 1.10f, 0.50f, 1.30f, 1.0f };

            PushBar(bBurning ? AdditiveQuads : AlphaQuads, { kFieldWidth * 0.5f, 148.0f }, 420.0f, FeverFill, Meter);

            if (bBurning)
            {
                Font::Emit(AdditiveQuads, "FEVER", { kFieldWidth * 0.5f, 100.0f }, 7.0f + Wobble * 2.0f,
                    Meter, 1.4f, Font::EAlign::Center);
            }
        }

        if (State.VaultGlow > 0.05f)
        {
            Buffer[0] = '\0';
            Append(Buffer, 64, "VAULT x");
            AppendInt(Buffer, 64, 1 + State.VaultScale());
            Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 560.0f }, 7.0f + Pulse(RealTime, 9.0f) * 1.5f,
                Fade(Gold, State.VaultGlow), 1.3f, Font::EAlign::Center);
        }

        if (State.BossIntro > 0.0f)
        {
            const float Alpha = Math::Min(1.0f, State.BossIntro * 1.5f);
            const float Rise = (2.6f - State.BossIntro) * 12.0f;
            Font::Emit(AdditiveQuads, "WARNING", { kFieldWidth * 0.5f, 400.0f - Rise }, 6.0f,
                Fade({ 1.60f, 0.25f, 0.30f, 1.0f }, Alpha * (0.5f + 0.5f * Pulse(RealTime, 12.0f))), 1.0f, Font::EAlign::Center);
            Font::Emit(AdditiveQuads, BossName(State.BossKind), { kFieldWidth * 0.5f, 460.0f - Rise }, 14.0f,
                Fade(Neon, Alpha), 1.4f, Font::EAlign::Center);
        }

        if (State.BreachWarning > 0.45f)
        {
            const float Alarm = 0.35f + 0.65f * Pulse(RealTime, 10.0f * State.BreachWarning);
            Font::Emit(AdditiveQuads, "BREACH", { kFieldWidth * 0.5f, 900.0f }, 8.0f,
                Fade({ 1.80f, 0.20f, 0.24f, 1.0f }, Alarm * State.BreachWarning), 1.2f, Font::EAlign::Center);
        }

        if (ShieldGlow >= 1.0f)
        {
            Font::Emit(AdditiveQuads, "SHIELD", { kFieldWidth - 58.0f, 1006.0f }, 4.0f,
                { 0.40f, 1.30f, 1.70f, 1.0f }, 0.7f, Font::EAlign::Right);
        }

        if (State.Danger > 0.35f)
        {
            Font::Emit(AdditiveQuads, "LAST LIFE", { kFieldWidth * 0.5f, 1024.0f }, 5.0f,
                Fade({ 1.60f, 0.20f, 0.28f, 1.0f }, 0.4f + 0.6f * Pulse(RealTime, 9.0f)), 1.0f, Font::EAlign::Center);
        }

        if (State.Combo >= 3)
        {
            Buffer[0] = 'x';
            Buffer[1] = '\0';
            AppendInt(Buffer, 64, State.Combo / 3 + 1);
            const float Heat = Math::Min(float(State.Combo) / 24.0f, 1.0f);
            const FVector4 ComboColor { 1.45f + Heat * 0.8f, 0.75f - Heat * 0.45f, 0.30f + Heat * 0.9f, 1.0f };
            const float Size = 9.0f + Heat * 7.0f + Pulse(RealTime, 16.0f) * Heat * 2.0f;

            Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 660.0f }, Size,
                Fade(ComboColor, Math::Min(1.0f, State.ComboTimer)), 1.1f + Heat, Font::EAlign::Center);
        }

        if (State.SlowTimer > 0.0f)
        {
            Font::Emit(AdditiveQuads, "SLOW MOTION", { kFieldWidth * 0.5f, 1030.0f }, 5.0f,
                Fade(Warm, 0.5f + 0.5f * Pulse(RealTime, 7.0f)), 0.8f, Font::EAlign::Center);
        }
        else if (State.FreezeTimer > 0.0f)
        {
            Font::Emit(AdditiveQuads, "FROZEN", { kFieldWidth * 0.5f, 1030.0f }, 5.0f,
                Fade({ 0.70f, 1.00f, 1.50f, 1.0f }, 0.5f + 0.5f * Pulse(RealTime, 5.0f)), 0.8f, Font::EAlign::Center);
        }

        const float Beat = 0.55f + 0.45f * Pulse(RealTime, 3.2f);

        switch (State.Phase)
        {
        case EPhase::Title:
        {
            Font::Emit(AdditiveQuads, "LUMINA", { kFieldWidth * 0.5f, 230.0f }, 7.0f, Neon, 0.9f, Font::EAlign::Center);
            Font::Emit(AdditiveQuads, "BREAKOUT", { kFieldWidth * 0.5f, 300.0f }, 24.0f, Bright, 1.2f, Font::EAlign::Center);

            const int32 Count = int32(EGameMode::Count);
            const float Spacing = 340.0f;
            const float StartX = kFieldWidth * 0.5f - Spacing * float(Count - 1) * 0.5f;
            for (int32 Index = 0; Index < Count; ++Index)
            {
                const EGameMode Mode = EGameMode(Index);
                const bool bSelected = Mode == State.MenuCursor;
                const float X = StartX + Spacing * float(Index);
                const float Size = bSelected ? 6.5f + Pulse(RealTime, 6.0f) * 0.6f : 4.5f;
                const FVector4 Ink = bSelected ? Warm : Fade(Label, 0.9f);
                Font::Emit(bSelected ? AdditiveQuads : AlphaQuads, GameModeName(Mode), { X, 520.0f }, Size, Ink,
                    bSelected ? 0.9f : 0.2f, Font::EAlign::Center);

                if (bSelected)
                {
                    PushPanel(AdditiveQuads, { X, 545.0f }, { Font::MeasureWidth(GameModeName(Mode), Size) * 0.5f + 20.0f, 2.0f },
                        Fade(Warm, Beat), 0.6f, 1.0f);
                }
            }

            Font::Emit(AlphaQuads, GameModeBlurb(State.MenuCursor), { kFieldWidth * 0.5f, 600.0f }, 3.5f,
                Fade(Bright, 0.9f), 0.2f, Font::EAlign::Center);

            Font::Emit(AdditiveQuads, "LEFT RIGHT TO PICK   SPACE TO PLAY", { kFieldWidth * 0.5f, 680.0f }, 6.0f,
                Fade(Warm, Beat), 0.9f, Font::EAlign::Center);
            Font::Emit(AlphaQuads, "MOUSE OR ARROW KEYS TO MOVE   SPACE ON CONTACT TO SMASH", { kFieldWidth * 0.5f, 760.0f }, 4.5f,
                Label, 0.2f, Font::EAlign::Center);
            Font::Emit(AlphaQuads, "P PAUSE   R RESTART   M MUTE   ESC QUIT", { kFieldWidth * 0.5f, 800.0f }, 4.5f,
                Label, 0.2f, Font::EAlign::Center);
            Font::Emit(AlphaQuads, "GET THE BALL BEHIND THE WALL FOR A VAULT BONUS", { kFieldWidth * 0.5f, 852.0f },
                4.0f, Fade(Gold, 0.9f), 0.3f, Font::EAlign::Center);
            Font::Emit(AlphaQuads, "RED PICKUPS HURT   THE WALL DESCENDS   DRONES DEFLECT", { kFieldWidth * 0.5f, 890.0f },
                4.0f, Fade({ 1.20f, 0.24f, 0.30f, 1.0f }, 0.9f), 0.4f, Font::EAlign::Center);
            break;
        }

        case EPhase::Serve:
            Font::Emit(AdditiveQuads, State.IsCoop() ? "ANY PLAYER PRESS SPACE TO LAUNCH" : "PRESS SPACE TO LAUNCH",
                { kFieldWidth * 0.5f, 870.0f }, 6.0f, Fade(Bright, Beat), 0.8f, Font::EAlign::Center);
            break;

        case EPhase::LevelClear:
        {
            Font::Emit(AdditiveQuads, "STAGE CLEAR", { kFieldWidth * 0.5f, 380.0f }, 18.0f, Bright, 1.4f, Font::EAlign::Center);

            const float Reveal = Math::Clamp((State.PhaseTimer - 0.6f) * 2.5f, 0.0f, 1.0f);
            if (Reveal > 0.0f)
            {
                const FVector4 Ink = GradeColor(State.Grade);
                const char GradeText[2] = { State.Grade, '\0' };
                Font::Emit(AdditiveQuads, "GRADE", { kFieldWidth * 0.5f - 120.0f, 560.0f }, 5.0f, Fade(Label, Reveal), 0.3f, Font::EAlign::Right);
                Font::Emit(AdditiveQuads, GradeText, { kFieldWidth * 0.5f + 20.0f, 520.0f }, 22.0f + (1.0f - Reveal) * 30.0f,
                    Fade(Ink, Reveal), 1.6f, Font::EAlign::Center);

                Buffer[0] = '\0';
                Append(Buffer, 64, "BONUS ");
                AppendInt(Buffer, 64, State.GradeBonus);
                Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 700.0f }, 5.0f, Fade(Ink, Reveal), 0.8f, Font::EAlign::Center);

                Buffer[0] = '\0';
                Append(Buffer, 64, "TIME ");
                AppendInt(Buffer, 64, int32(State.LevelTime));
                Append(Buffer, 64, "S   COMBO ");
                AppendInt(Buffer, 64, State.LevelBestCombo);
                Append(Buffer, 64, "   SMASHES ");
                AppendInt(Buffer, 64, State.LevelSmashes);
                Append(Buffer, 64, "   LOST ");
                AppendInt(Buffer, 64, State.LevelLivesLost);
                Font::Emit(AlphaQuads, Buffer, { kFieldWidth * 0.5f, 760.0f }, 4.0f, Fade(Label, Reveal), 0.2f, Font::EAlign::Center);
            }
            break;
        }

        case EPhase::Draft:
        {
            Font::Emit(AdditiveQuads, "CHOOSE A PERK", { kFieldWidth * 0.5f, 250.0f }, 12.0f, Bright, 1.2f, Font::EAlign::Center);

            const float CardWidth = 540.0f;
            const float Spacing = 600.0f;
            const float StartX = kFieldWidth * 0.5f - Spacing;
            for (int32 Index = 0; Index < kDraftChoices; ++Index)
            {
                const EPerk Perk = State.Draft[Index];
                const FPerkInfo Info = PerkInfo(Perk);
                const bool bSelected = Index == State.DraftCursor;
                const float X = StartX + Spacing * float(Index);
                const float Lift = bSelected ? 14.0f + Pulse(RealTime, 5.0f) * 6.0f : 0.0f;
                const FVector2 Center { X, 560.0f - Lift };

                const FVector4 Frame = bSelected ? Warm : Fade(Label, 0.55f);
                PushPanel(AlphaQuads, Center, { CardWidth * 0.5f + 6.0f, 156.0f }, Fade(Frame, bSelected ? 0.9f : 0.5f), bSelected ? 0.9f : 0.1f);
                PushPanel(AlphaQuads, Center, { CardWidth * 0.5f, 150.0f }, { 0.05f, 0.06f, 0.14f, 0.96f }, 0.0f);

                Buffer[0] = '\0';
                AppendInt(Buffer, 64, Index + 1);
                Font::Emit(AlphaQuads, Buffer, { Center.x - CardWidth * 0.5f + 24.0f, Center.y - 130.0f }, 4.0f, Fade(Label, 0.9f), 0.2f);

                Font::Emit(bSelected ? AdditiveQuads : AlphaQuads, Info.Name, { Center.x, Center.y - 90.0f }, bSelected ? 8.5f : 7.5f,
                    bSelected ? Gold : Bright, bSelected ? 1.0f : 0.3f, Font::EAlign::Center);
                Font::Emit(AlphaQuads, Info.Blurb, { Center.x, Center.y + 4.0f }, 3.0f, Fade(Bright, 0.95f), 0.2f, Font::EAlign::Center);

                const int32 Owned = State.Perks.Count(Perk);
                Buffer[0] = '\0';
                if (Owned > 0)
                {
                    Append(Buffer, 64, "OWNED x");
                    AppendInt(Buffer, 64, Owned);
                    Append(Buffer, 64, "   MAX ");
                    AppendInt(Buffer, 64, Info.MaxStacks);
                }
                else if (Perk == EPerk::Lifeline)
                {
                    Append(Buffer, 64, "ONE TIME");
                }
                else
                {
                    Append(Buffer, 64, "MAX ");
                    AppendInt(Buffer, 64, Info.MaxStacks);
                }
                Font::Emit(AlphaQuads, Buffer, { Center.x, Center.y + 90.0f }, 3.0f, Fade(Label, 0.9f), 0.2f, Font::EAlign::Center);
            }

            Font::Emit(AdditiveQuads, "1 2 3 OR ARROWS THEN SPACE", { kFieldWidth * 0.5f, 800.0f }, 5.5f,
                Fade(Warm, Beat), 0.8f, Font::EAlign::Center);
            break;
        }

        case EPhase::GameOver:
        {
            Font::Emit(AdditiveQuads, "GAME OVER", { kFieldWidth * 0.5f, 300.0f }, 22.0f, Neon, 1.3f, Font::EAlign::Center);
            Font::Emit(AlphaQuads, "FINAL SCORE", { kFieldWidth * 0.5f, 520.0f }, 5.0f, Label, 0.2f, Font::EAlign::Center);
            Font::FormatInt(Buffer, 64, State.Score);
            Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 570.0f }, 11.0f, Bright, 1.0f, Font::EAlign::Center);

            Buffer[0] = '\0';
            Append(Buffer, 64, "STAGE ");
            AppendInt(Buffer, 64, State.Level);
            Append(Buffer, 64, "   BEST COMBO ");
            AppendInt(Buffer, 64, State.BestCombo);
            Append(Buffer, 64, "   BRICKS ");
            AppendInt(Buffer, 64, State.BricksBroken);
            Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 680.0f }, 4.5f, Warm, 0.5f, Font::EAlign::Center);

            Buffer[0] = '\0';
            Append(Buffer, 64, "SMASHES ");
            AppendInt(Buffer, 64, State.Smashes);
            Append(Buffer, 64, "   VAULTS ");
            AppendInt(Buffer, 64, State.Vaults);
            Append(Buffer, 64, "   DRONES ");
            AppendInt(Buffer, 64, State.DronesDowned);
            Font::Emit(AdditiveQuads, Buffer, { kFieldWidth * 0.5f, 725.0f }, 4.5f, Warm, 0.5f, Font::EAlign::Center);

            Font::Emit(AdditiveQuads, "SPACE FOR MENU   R TO RETRY", { kFieldWidth * 0.5f, 800.0f }, 7.0f,
                Fade(Warm, Beat), 0.9f, Font::EAlign::Center);
            break;
        }

        default:
            break;
        }

        if (State.HighScore > 0 && State.Phase != EPhase::Playing)
        {
            Font::Emit(AlphaQuads, "BEST", { 58.0f, 150.0f }, 4.0f, Label, 0.2f);
            Font::FormatPadded(Buffer, 64, State.HighScore, 7);
            Font::Emit(AlphaQuads, Buffer, { 58.0f, 180.0f }, 5.0f, Fade(Label, 1.4f), 0.3f);
        }

        if (Game.ShowsStats())
        {
            const FFrameStats& Stats = Game.GetStats();
            const FVector4 Ink { 0.35f, 1.10f, 0.55f, 1.0f };

            struct FStatRow
            {
                const char* Name;
                int32       Value;
            };

            const FStatRow Rows[7] =
            {
                { "MS",     int32(Stats.FrameMilliseconds * 100.0f) },
                { "WORST",  int32(Stats.WorstMilliseconds * 100.0f) },
                { "STEPS",  Stats.SimSteps },
                { "DROP",   Stats.DroppedSteps },
                { "PARTS",  Stats.Particles },
                { "ENTS",   Stats.Entities },
                { "QUADS",  Stats.AlphaQuads + Stats.AdditiveQuads },
            };

            float StatY = 240.0f;
            for (const FStatRow& Row : Rows)
            {
                Buffer[0] = '\0';
                Append(Buffer, 64, Row.Name);
                Append(Buffer, 64, " ");
                AppendInt(Buffer, 64, Row.Value);
                Font::Emit(AlphaQuads, Buffer, { 58.0f, StatY }, 4.0f, Ink, 0.2f);
                StatY += 34.0f;
            }
        }

        if (Game.IsPaused())
        {
            Font::Emit(AdditiveQuads, "PAUSED", { kFieldWidth * 0.5f, 420.0f }, 22.0f, Bright, 1.2f, Font::EAlign::Center);
            Font::Emit(AdditiveQuads, "P RESUME   ESC QUIT", { kFieldWidth * 0.5f, 640.0f }, 7.0f,
                Fade(Warm, Beat), 0.8f, Font::EAlign::Center);
        }
    }

    void FRenderer::DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, float RealTime, const FGameState& State)
    {
        RHI::CmdBeginMarker(CL, "Breakout.Scene");
        RHI::Utils::BeginScreenPass(CL, { .Target = SceneTarget.Texture, .Extent = Extent, });

        const FBackgroundArgs Background
        {
            .Resolution  = { float(Extent.x), float(Extent.y) },
            .FieldOrigin = FieldOriginPixels,
            .FieldSize   = FieldSizePixels,
            .Time        = RealTime,
            .Flash       = State.FlashPulse,
            .Level       = float(State.Level),
            .Beat        = BeatPulse,
            .Danger      = State.Danger,
            .Progress    = State.Progress,
            .Fever       = State.FeverTimer > 0.0f ? Math::Min(1.0f, State.FeverTimer * 2.0f) : 0.0f,
        };

        RHI::Utils::DrawFullscreen(CL, BackgroundPipeline, RHI::CopyTransient(Background));

        const RHI::FRect FieldRect
        {
            Math::Clamp(int32(FieldOriginPixels.x), 0, int32(Extent.x)),
            Math::Clamp(int32(FieldOriginPixels.x + FieldSizePixels.x), 0, int32(Extent.x)),
            Math::Clamp(int32(FieldOriginPixels.y), 0, int32(Extent.y)),
            Math::Clamp(int32(FieldOriginPixels.y + FieldSizePixels.y), 0, int32(Extent.y)),
        };
        RHI::CmdSetScissor(CL, FieldRect);

        FQuadArgs Args
        {
            .Instances     = 0,
            .NdcScale      = { 2.0f / float(Extent.x), 2.0f / float(Extent.y) },
            .NdcOffset     = { FieldOriginPixels.x * 2.0f / float(Extent.x) - 1.0f,
                               FieldOriginPixels.y * 2.0f / float(Extent.y) - 1.0f },
            .UnitsToPixels = { UnitsToPixels, UnitsToPixels },
            .Time          = RealTime,
        };

        if (!AlphaQuads.empty())
        {
            Args.Instances = RHI::CopyTransientArray(AlphaQuads.data(), AlphaQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAlphaPipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(Args), 6, uint32(AlphaQuads.size()), 0, 0);
        }

        if (!AdditiveQuads.empty())
        {
            Args.Instances = RHI::CopyTransientArray(AdditiveQuads.data(), AdditiveQuads.size()).Address;
            RHI::CmdSetPipeline(CL, QuadAdditivePipeline);
            RHI::CmdDraw(CL, RHI::CopyTransient(Args), 6, uint32(AdditiveQuads.size()), 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);
    }

    void FRenderer::DrawBloom(RHI::FCmdListH CL)
    {
        RHI::CmdBeginMarker(CL, "Breakout.Bloom");

        for (int32 Level = 0; Level < kBloomLevels; ++Level)
        {
            const uint32 SourceID = Level == 0 ? SceneTarget.SampledSlot : BloomChain.SampledSlot(Level - 1);
            const FVector2 SourceTexel = Level == 0
                ? FVector2 { 1.0f / float(TargetExtent.x), 1.0f / float(TargetExtent.y) }
                : BloomChain.TexelSize(Level - 1);

            const FBloomArgs Args
            {
                .SourceID        = SourceID,
                .bFirstPass      = Level == 0 ? 1u : 0u,
                .SourceTexelSize = SourceTexel,
                .Threshold       = 1.05f,
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
                .Intensity       = 0.60f,
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
                                  float RealTime, const FGameState& State, const FCameraShake& Shake, bool bPaused)
    {
        const FCompositeArgs Args
        {
            .SceneID        = SceneTarget.SampledSlot,
            .BloomID        = BloomChain.SampledSlot(0),
            .Resolution     = { float(Extent.x), float(Extent.y) },
            .BloomIntensity = 0.17f,
            .Exposure       = 0.95f,
            .Chroma         = Shake.Chroma,
            .Vignette       = 0.55f,
            .Time           = RealTime,
            .Flash          = State.FlashPulse,
            .Fade           = bPaused ? 0.45f : 1.0f,
            .Danger         = State.Danger,
            .Fire           = State.FireGlow,
            .Beat           = BeatPulse,
            .Fever          = State.FeverTimer > 0.0f ? Math::Min(1.0f, State.FeverTimer * 2.0f) : 0.0f,
            .Zoom           = 1.0f + State.VaultGlow * 0.035f,
            .Blind          = Math::Min(1.0f, State.BlindTimer * 2.0f),
            .Vault          = State.VaultGlow,
            .Waves          = { Warps[0], Warps[1], Warps[2], Warps[3] },
        };

        RHI::CmdBeginMarker(CL, "Breakout.Composite");
        RHI::Utils::BeginScreenPass(CL, { .Target = SwapImage, .Extent = Extent });
        RHI::Utils::DrawFullscreen(CL, CompositePipeline, RHI::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
    }
}
