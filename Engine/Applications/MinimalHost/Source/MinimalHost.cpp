#include "Audio/AudioContext.h"
#include "Audio/AudioGlobals.h"
#include "Audio/ProceduralAudioStream.h"
#include "Core/Application/ApplicationGlobalState.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Math/Math.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowTypes.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/SwapchainTarget.h"
#include "TaskSystem/TaskSystem.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kToneRate     = 48000;
    constexpr uint32 kToneChannels = 1;
    constexpr uint32 kToneFrames   = 8192;

    struct FHostState
    {
        float Hue     = 0.55f;
        bool  bQuit   = false;
    };

    // Input arrives on the window itself, so a host needs no application object to read the keyboard.
    void BindInput(FWindow& Window, FHostState& State)
    {
        (void)Window.OnKey.AddLambda([&State](FWindow*, const FKeyInput& Input)
        {
            if (!Input.bPressed || Input.bRepeat)
            {
                return;
            }

            if (Input.Key == EKey::Escape)
            {
                State.bQuit = true;
            }
            else
            {
                State.Hue = Math::Fract(State.Hue + 0.13f);
            }
        });

        (void)Window.OnCloseRequested.AddLambda([&State](FWindow*)
        {
            State.bQuit = true;
        });
    }

    FVector3 HueToColor(float Hue)
    {
        const float R = Math::Abs(Hue * 6.0f - 3.0f) - 1.0f;
        const float G = 2.0f - Math::Abs(Hue * 6.0f - 2.0f);
        const float B = 2.0f - Math::Abs(Hue * 6.0f - 4.0f);
        return { Math::Clamp(R, 0.0f, 1.0f), Math::Clamp(G, 0.0f, 1.0f), Math::Clamp(B, 0.0f, 1.0f) };
    }

    void PushTone(FProceduralAudioStream& Stream, float& Phase, float Frequency)
    {
        const uint32 Room = Math::Min(Stream.GetAvailableWriteFrames(), 2048u);
        if (Room == 0)
        {
            return;
        }

        float Samples[2048];
        const float Step = Frequency / float(kToneRate);

        for (uint32 Index = 0; Index < Room; ++Index)
        {
            Samples[Index] = Math::Sin(Phase * Math::TwoPi<float>()) * 0.12f;
            Phase = Math::Fract(Phase + Step);
        }

        (void)Stream.Write(Samples, Room);
    }
}

int main(int ArgC, char** ArgV)
{
    Memory::Initialize();

    FApplicationGlobalState GlobalState("MinimalHost Main");
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    FWindow Window(FWindowSpecs{ .Title = "Lumina Minimal Host", .Extent = { 960, 540 } });

    FHostState State;
    BindInput(Window, State);

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });

    RHI::FSwapchainTarget Target;
    Target.Initialize(RHI::CreateSurface(Window.GetWindow()), Window.GetExtent());

    Audio::Initialize();

    TSharedPtr<FProceduralAudioStream> Tone;
    float TonePhase = 0.0f;

    if (Audio::HasDevice())
    {
        Tone = Audio::Context().CreateProceduralStream(kToneRate, kToneChannels, kToneFrames);
        if (Tone)
        {
            FAudioPlayParams Params;
            Params.Volume = 0.6f;
            (void)Audio::Context().PlayProceduralStream(Tone, Params);
        }
        LOG_INFO("MinimalHost: audio online.");
    }
    else
    {
        LOG_WARN("MinimalHost: no audio endpoint, running silent.");
    }

    const double Started = PlatformTime::Seconds();

    for (uint32 FrameSlot = 0; !Window.ShouldClose() && !State.bQuit;
         FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
    {
        RHI::BeginFrame(FrameSlot);

        Window.ProcessMessages();
        Target.Resize(Window.GetExtent());

        Audio::Update();

        if (Tone)
        {
            PushTone(*Tone, TonePhase, 220.0f + State.Hue * 440.0f);
        }

        // A headless run exits on its own, so this target can be built and executed by a script.
        if (ParsedCommandLine.Has("exitafter") && PlatformTime::Seconds() - Started > 3.0)
        {
            break;
        }

        const RHI::FTextureH SwapImage = Target.Acquire();
        if (!RHI::IsValid(SwapImage))
        {
            continue;
        }

        const FVector3 Color = HueToColor(State.Hue);
        const FUIntVector2 Extent = Target.GetExtent();

        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        Target.BarrierToRender(CL);

        const RHI::FRenderAttachment Attachment
        {
            .Texture = SwapImage,
            .LoadOp  = RHI::ELoadOp::Clear,
            .StoreOp = RHI::EStoreOp::Store,
            .Color   = { Color.x * 0.25f, Color.y * 0.25f, Color.z * 0.25f, 1.0f },
        };
        const RHI::FRenderPassDesc Pass
        {
            .ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Attachment, 1),
            .RenderArea       = Extent,
        };

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdEndRenderPass(CL);

        Target.Present(CL);
    }

    Tone.reset();
    Audio::Shutdown();

    RHI::WaitDeviceIdle();
    Target.Shutdown();

    RHI::FreeDevice();

    Task::Shutdown();
    GCommandLine = nullptr;

    LOG_INFO("MinimalHost: clean shutdown.");
    return 0;
}
