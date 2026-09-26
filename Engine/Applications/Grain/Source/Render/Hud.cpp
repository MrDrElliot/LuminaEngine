#include "Hud.h"
#include "HudFont.h"
#include "ShaderHud.h"

#include "Game/Game.h"
#include "Containers/StringFormat.h"
#include "Log/Log.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHIUtils.h"
#include "Renderer/ShaderCompiler.h"

#include <cstring>

namespace Grain
{
    namespace
    {
        struct FHudArgs
        {
            RHI::GPUPtr Vertices = 0;
            RHI::GPUPtr Font = 0;
            FVector2    Resolution { 1.0f, 1.0f };
            FVector2    Pad { 0.0f, 0.0f };
        };

        static_assert(sizeof(FHudArgs) == 32, "Slang mirror expects a packed 32 byte block.");

        constexpr uint32 kSolidGlyph = 0xFFFFFFFFu;

        const FVector4 kInk      { 0.92f, 0.94f, 0.98f, 1.00f };
        const FVector4 kDim      { 0.62f, 0.66f, 0.74f, 1.00f };
        const FVector4 kPanel    { 0.04f, 0.05f, 0.07f, 0.62f };
        const FVector4 kEdge     { 0.55f, 0.62f, 0.74f, 0.34f };
        const FVector4 kHealth   { 0.82f, 0.22f, 0.20f, 1.00f };
        const FVector4 kStamina  { 0.36f, 0.72f, 0.34f, 1.00f };
        const FVector4 kFocus    { 0.30f, 0.62f, 0.92f, 1.00f };
        const FVector4 kShard    { 0.42f, 0.86f, 0.98f, 1.00f };
        const FVector4 kGoldInk  { 0.98f, 0.80f, 0.36f, 1.00f };
        const FVector4 kTrack    { 0.10f, 0.11f, 0.14f, 0.78f };

        FVector4 Fade(const FVector4& Color, float Alpha)
        {
            return { Color.x, Color.y, Color.z, Color.w * Alpha };
        }

        TVector<uint32> CompileHud(const char* EntryPoint, const char* DebugName)
        {
            TVector<uint32> Spirv;

            FShaderCompileOptions Options;
            Options.DebugName = DebugName;
            Options.EntryPoint = EntryPoint;
            Options.bGenerateReflectionData = false;

            GShaderCompiler->CompilerShaderRaw(FString(Shaders::kHudModule), Options,
                [&Spirv](FShaderHeader Header) { Spirv = Move(Header.Binaries); });

            GShaderCompiler->Flush();

            if (Spirv.empty())
            {
                LOG_ERROR("Grain: failed to compile '{}'.", DebugName);
            }
            return Spirv;
        }

    }

    bool FHud::Initialize(EFormat InSwapchainFormat)
    {
        SwapchainFormat = InSwapchainFormat;

        const TVector<uint32> Vertex = CompileHud("HudVS", "Grain.HudVS");
        const TVector<uint32> Pixel = CompileHud("HudPS", "Grain.HudPS");

        if (Vertex.empty() || Pixel.empty())
        {
            return false;
        }

        RHI::FColorTarget ColorTarget;
        ColorTarget.Format = SwapchainFormat;
        ColorTarget.Blend.bBlendEnable = true;
        ColorTarget.Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        ColorTarget.Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        ColorTarget.Blend.SrcAlphaFactor = RHI::EFactor::One;
        ColorTarget.Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        RHI::FRasterDesc Raster;
        Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        const auto Source = [](const TVector<uint32>& Spirv, const char* Entry)
        {
            return RHI::FShaderSource
            {
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()),
                                                     Spirv.size() * sizeof(uint32)),
                .EntryPoint = Entry,
            };
        };

        Pipeline = RHI::CreateGraphicsPipeline(Source(Vertex, "HudVS"), Source(Pixel, "HudPS"), Raster);

        VertexBuffer = RHI::Malloc(uint64(kMaxHudVertices) * sizeof(FHudVertex), RHI::EMemoryType::CPUWrite);

        constexpr uint32 kFontWords = (kFontGlyphCount * kFontColumns + 3) / 4;
        FontBuffer = RHI::Malloc(uint64(kFontWords) * sizeof(uint32), RHI::EMemoryType::CPUWrite);

        if (!RHI::IsValid(Pipeline) || VertexBuffer.Cpu == nullptr || FontBuffer.Cpu == nullptr)
        {
            LOG_ERROR("Grain: overlay initialization failed.");
            return false;
        }

        RHI::SetDebugName(VertexBuffer.Gpu, "Grain.HudVertices");
        RHI::SetDebugName(FontBuffer.Gpu, "Grain.HudFont");

        uint32* Font = FontBuffer.CpuAs<uint32>();
        std::memset(Font, 0, size_t(kFontWords) * sizeof(uint32));

        for (uint32 i = 0; i < kFontGlyphCount * kFontColumns; ++i)
        {
            Font[i >> 2] |= uint32(kFontColumnBits[i]) << ((i & 3u) * 8u);
        }

        Vertices.reserve(4096);
        return true;
    }

    void FHud::Quad(float X, float Y, float Width, float Height, const FVector4& Color)
    {
        if (Vertices.size() + 6 > kMaxHudVertices || Color.w <= 0.001f)
        {
            return;
        }

        const float Corners[6][2] =
        {
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
            { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
        };

        for (const auto& Corner : Corners)
        {
            FHudVertex Vertex;
            Vertex.Color = Color;
            Vertex.Position = { X + Corner[0] * Width, Y + Corner[1] * Height };
            Vertex.Local = { Corner[0], Corner[1] };
            Vertex.Glyph = kSolidGlyph;
            Vertices.push_back(Vertex);
        }
    }

    void FHud::Frame(float X, float Y, float Width, float Height, float Thickness, const FVector4& Color)
    {
        Quad(X, Y, Width, Thickness, Color);
        Quad(X, Y + Height - Thickness, Width, Thickness, Color);
        Quad(X, Y, Thickness, Height, Color);
        Quad(X + Width - Thickness, Y, Thickness, Height, Color);
    }

    void FHud::Bar(float X, float Y, float Width, float Height, float Fill,
                   const FVector4& Color, const FVector4& Back)
    {
        Quad(X, Y, Width, Height, Back);
        Quad(X, Y, Width * Math::Clamp(Fill, 0.0f, 1.0f), Height, Color);
        Frame(X, Y, Width, Height, 1.0f, kEdge);
    }

    void FHud::Glyph(char Character, float X, float Y, float Scale, const FVector4& Color)
    {
        const uint32 Code = uint32(uint8(Character));
        if (Code < kFontFirstGlyph || Code >= kFontFirstGlyph + kFontGlyphCount)
        {
            return;
        }

        if (Vertices.size() + 6 > kMaxHudVertices || Color.w <= 0.001f)
        {
            return;
        }

        const float Width = float(kFontColumns) * Scale;
        const float Height = float(kFontRows) * Scale;

        const float Corners[6][2] =
        {
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
            { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
        };

        for (const auto& Corner : Corners)
        {
            FHudVertex Vertex;
            Vertex.Color = Color;
            Vertex.Position = { X + Corner[0] * Width, Y + Corner[1] * Height };
            Vertex.Local = { Corner[0], Corner[1] };
            Vertex.Glyph = Code - kFontFirstGlyph;
            Vertices.push_back(Vertex);
        }
    }

    float FHud::TextWidth(const char* Value, float Scale)
    {
        float Width = 0.0f;
        for (const char* Cursor = Value; *Cursor != 0; ++Cursor)
        {
            Width += float(kFontColumns + 1) * Scale;
        }
        return Math::Max(Width - Scale, 0.0f);
    }

    void FHud::Text(const char* Value, float X, float Y, float Scale, const FVector4& Color)
    {
        float Cursor = X;
        for (const char* Character = Value; *Character != 0; ++Character)
        {
            if (*Character != ' ')
            {
                // A dark copy one pixel down is what keeps text readable over snow and sky alike.
                Glyph(*Character, Cursor + Scale, Y + Scale, Scale, Fade({ 0.0f, 0.0f, 0.0f, 1.0f }, Color.w * 0.7f));
                Glyph(*Character, Cursor, Y, Scale, Color);
            }
            Cursor += float(kFontColumns + 1) * Scale;
        }
    }

    void FHud::TextCentered(const char* Value, float CenterX, float Y, float Scale, const FVector4& Color)
    {
        Text(Value, CenterX - TextWidth(Value, Scale) * 0.5f, Y, Scale, Color);
    }

    void FHud::Build(const FGame& Game, const FUIntVector2& Extent, float RealTime)
    {
        Vertices.clear();

        if (!bVisible)
        {
            return;
        }

        const float Width = float(Extent.x);
        const float Height = float(Extent.y);
        const float Unit = Math::Max(Math::Floor(Height / 300.0f), 2.0f);

        const FPlayerState& Player = Game.GetPlayer();

        //~ Crosshair.

        {
            const float Size = Unit * 4.0f;
            const float Thick = Math::Max(Unit * 0.5f, 1.0f);
            const FVector4 Color = Fade(kInk, 0.55f);

            Quad(Width * 0.5f - Size, Height * 0.5f - Thick * 0.5f, Size * 0.7f, Thick, Color);
            Quad(Width * 0.5f + Size * 0.3f, Height * 0.5f - Thick * 0.5f, Size * 0.7f, Thick, Color);
            Quad(Width * 0.5f - Thick * 0.5f, Height * 0.5f - Size, Thick, Size * 0.7f, Color);
            Quad(Width * 0.5f - Thick * 0.5f, Height * 0.5f + Size * 0.3f, Thick, Size * 0.7f, Color);
        }

        //~ Vitals, bottom left.

        {
            const float PanelWidth = Unit * 80.0f;
            const float PanelHeight = Unit * 34.0f;
            const float X = Unit * 6.0f;
            const float Y = Height - PanelHeight - Unit * 6.0f;

            Quad(X, Y, PanelWidth, PanelHeight, kPanel);
            Frame(X, Y, PanelWidth, PanelHeight, 1.0f, kEdge);

            const float BarX = X + Unit * 4.0f;
            const float BarWidth = PanelWidth - Unit * 8.0f;
            const float BarHeight = Unit * 5.0f;

            Bar(BarX, Y + Unit * 4.0f, BarWidth, BarHeight,
                Player.Health / Math::Max(Player.MaxHealth, 1.0f), kHealth, kTrack);
            Bar(BarX, Y + Unit * 12.0f, BarWidth, BarHeight * 0.7f,
                Player.Stamina / Math::Max(Player.MaxStamina, 1.0f), kStamina, kTrack);
            Bar(BarX, Y + Unit * 18.0f, BarWidth, BarHeight * 0.7f,
                Player.Focus / Math::Max(Player.MaxFocus, 1.0f), kFocus, kTrack);

            const FString Vitals = FString("HP ").append(Format("{}", int32(Player.Health)))
                .append("/").append(Format("{}", int32(Player.MaxHealth)));
            Text(Vitals.c_str(), BarX, Y + Unit * 25.0f, Unit * 0.9f, kInk);

            const FString LevelText = FString("LV ").append(Format("{}", Player.Level));
            Text(LevelText.c_str(), X + PanelWidth - Unit * 22.0f, Y + Unit * 25.0f, Unit * 0.9f, kGoldInk);
        }

        //~ Experience, along the very bottom.

        {
            const float BarHeight = Unit * 2.0f;
            const float Fill = float(Player.Experience) / float(Math::Max(Game.GetExperienceForNext(), 1));
            Quad(0.0f, Height - BarHeight, Width, BarHeight, kTrack);
            Quad(0.0f, Height - BarHeight, Width * Math::Clamp(Fill, 0.0f, 1.0f), BarHeight, kGoldInk);
        }

        //~ Carried shards and the tally, top left.

        {
            const float X = Unit * 6.0f;
            const float Y = Unit * 6.0f;
            const float PanelWidth = Unit * 62.0f;
            const float PanelHeight = Unit * 24.0f;

            Quad(X, Y, PanelWidth, PanelHeight, kPanel);
            Frame(X, Y, PanelWidth, PanelHeight, 1.0f, kEdge);

            const FString Shards = FString("SHARDS  ").append(Format("{}", Player.Shards));
            Text(Shards.c_str(), X + Unit * 4.0f, Y + Unit * 4.0f, Unit * 1.1f, kShard);

            const FString Kills = FString("FELLED  ").append(Format("{}", Player.Kills));
            Text(Kills.c_str(), X + Unit * 4.0f, Y + Unit * 13.0f, Unit * 1.1f, kDim);
        }

        //~ Objectives, top right.

        {
            const float PanelWidth = Unit * 132.0f;
            const float X = Width - PanelWidth - Unit * 6.0f;
            const float Y = Unit * 6.0f;

            const TVector<FObjective>& Objectives = Game.GetObjectives();
            const float PanelHeight = Unit * (12.0f + float(Objectives.size()) * 9.0f);

            Quad(X, Y, PanelWidth, PanelHeight, kPanel);
            Frame(X, Y, PanelWidth, PanelHeight, 1.0f, kEdge);

            Text("EMBER VALE", X + Unit * 4.0f, Y + Unit * 3.0f, Unit * 1.0f, kGoldInk);

            float Line = Y + Unit * 12.0f;
            for (const FObjective& Objective : Objectives)
            {
                const FVector4 Color = Objective.bComplete ? kStamina : kInk;

                FString Row = Objective.bComplete ? FString("x ") : FString("- ");
                Row.append(Objective.Text);
                Row.append(" ").append(Format("{}", Objective.Progress));
                Row.append("/").append(Format("{}", Objective.Target));

                Text(Row.c_str(), X + Unit * 4.0f, Line, Unit * 0.8f, Color);
                Line += Unit * 9.0f;
            }
        }

        //~ Clock, top center.

        {
            const float Phase = Game.GetTimeOfDay();
            const float Shifted = Phase + 0.25f;
            const int32 Hour = int32((Shifted - Math::Floor(Shifted)) * 24.0f);
            const FString Clock = FString(Hour < 10 ? "0" : "").append(Format("{}", Hour)).append("00");

            const bool bDay = Game.GetSky().DayFactor > 0.4f;
            TextCentered(Clock.c_str(), Width * 0.5f, Unit * 6.0f, Unit * 1.1f, bDay ? kGoldInk : kShard);
        }

        //~ Floating damage and pickup text, projected through the same basis the shaders use.

        {
            const FVector3 Eye = Game.GetCameraPosition();
            const FVector3 Forward = Game.GetLookDirection();
            const FVector3 Right { -Math::Sin(Player.Yaw), 0.0f, Math::Cos(Player.Yaw) };
            const FVector3 Up
            {
                Right.y * Forward.z - Right.z * Forward.y,
                Right.z * Forward.x - Right.x * Forward.z,
                Right.x * Forward.y - Right.y * Forward.x,
            };

            const float TanHalfFov = Math::Tan(0.5f * 1.20f);
            const float Aspect = Width / Math::Max(Height, 1.0f);

            for (const FFloatingText& Entry : Game.GetFloatingText())
            {
                const float Life = Math::Clamp(1.0f - Entry.Age / Math::Max(Entry.Life, 0.01f), 0.0f, 1.0f);

                const FVector3 Rel
                {
                    Entry.World.x - Eye.x,
                    Entry.World.y - Eye.y + Entry.Age * 1.3f,
                    Entry.World.z - Eye.z,
                };

                const float Depth = Rel.x * Forward.x + Rel.y * Forward.y + Rel.z * Forward.z;
                if (Depth < 0.6f || Depth > 60.0f)
                {
                    continue;
                }

                const float SideX = Rel.x * Right.x + Rel.y * Right.y + Rel.z * Right.z;
                const float SideY = Rel.x * Up.x + Rel.y * Up.y + Rel.z * Up.z;

                const float Ndc = SideX / (Depth * TanHalfFov * Aspect);
                const float NdcY = -SideY / (Depth * TanHalfFov);

                if (Math::Abs(Ndc) > 1.0f || Math::Abs(NdcY) > 1.0f)
                {
                    continue;
                }

                const float ScreenX = (Ndc * 0.5f + 0.5f) * Width;
                const float ScreenY = (NdcY * 0.5f + 0.5f) * Height;

                const FVector4 Color { Entry.Color.x, Entry.Color.y, Entry.Color.z, Life };
                TextCentered(Entry.Text.c_str(), ScreenX, ScreenY, Unit * 1.0f, Color);
            }
        }

        //~ Prompt and banner.

        if (Game.GetPrompt()[0] != 0)
        {
            const float Pulse = 0.75f + 0.25f * Math::Sin(RealTime * 4.0f);
            TextCentered(Game.GetPrompt(), Width * 0.5f, Height * 0.62f, Unit * 1.2f, Fade(kGoldInk, Pulse));
        }

        const FBanner& Banner = Game.GetBanner();
        if (Banner.Life > 0.0f)
        {
            const float Remaining = 1.0f - Banner.Age / Banner.Life;
            const float Alpha = Math::Min(Banner.Age * 4.0f, Math::Min(Remaining * 4.0f, 1.0f));
            TextCentered(Banner.Text.c_str(), Width * 0.5f, Height * 0.22f, Unit * 2.0f, Fade(kInk, Alpha));
        }

        if (!Player.bAlive)
        {
            Quad(0.0f, 0.0f, Width, Height, { 0.25f, 0.02f, 0.02f, 0.42f });
            TextCentered("YOU FALL", Width * 0.5f, Height * 0.42f, Unit * 3.4f, kHealth);
        }
        else if (Player.HurtTimer > 0.0f)
        {
            const float Alpha = Player.HurtTimer / 0.45f;
            Quad(0.0f, 0.0f, Width, Height * 0.10f, { 0.6f, 0.05f, 0.05f, Alpha * 0.4f });
            Quad(0.0f, Height * 0.90f, Width, Height * 0.10f, { 0.6f, 0.05f, 0.05f, Alpha * 0.4f });
        }

        if (Game.IsComplete())
        {
            TextCentered("THE VALE IS YOURS", Width * 0.5f, Height * 0.34f, Unit * 2.6f, kGoldInk);
        }
    }

    void FHud::Draw(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent)
    {
        if (Vertices.empty() || !RHI::IsValid(Pipeline) || VertexBuffer.Cpu == nullptr)
        {
            return;
        }

        const uint32 Count = uint32(Math::Min<size_t>(Vertices.size(), kMaxHudVertices));
        std::memcpy(VertexBuffer.Cpu, Vertices.data(), size_t(Count) * sizeof(FHudVertex));

        FHudArgs Args;
        Args.Vertices = VertexBuffer.Gpu;
        Args.Font = FontBuffer.Gpu;
        Args.Resolution = { float(Extent.x), float(Extent.y) };

        RHI::CmdBeginMarker(CL, "Grain.Hud");

        // The composite wrote this same attachment in the pass just before, so the write has to land.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ColorWrite,
            RHI::EStageFlags::RasterColorOut,
            RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);

        RHI::Utils::BeginScreenPass(CL, { .Target = Target, .Extent = Extent,
            .LoadOp = RHI::ELoadOp::Load });

        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDraw(CL, RHI::CopyTransient(Args), Count, 1, 0, 0);

        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
    }

    void FHud::Shutdown()
    {
        if (RHI::IsValid(Pipeline))
        {
            RHI::Retire(Pipeline);
            Pipeline = {};
        }

        for (RHI::FGPUAllocation* Allocation : { &VertexBuffer, &FontBuffer })
        {
            if (Allocation->Gpu != 0)
            {
                RHI::Retire(*Allocation);
                *Allocation = {};
            }
        }
    }
}
