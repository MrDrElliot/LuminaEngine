#include "Audio/SoundEngine.h"
#include "Game/Game.h"
#include "Render/Renderer.h"

#include "Core/Application/ApplicationGlobalState.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowTypes.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/SwapchainTarget.h"
#include "TaskSystem/TaskSystem.h"

using namespace Lumina;
using namespace Breakout;

namespace
{
    void BindInput(FWindow& Window, FGame& Game)
    {
        (void)Window.OnKey.AddLambda([&Game](FWindow*, const FKeyInput& Input)
        {
            if (!Input.bRepeat)
            {
                Game.OnKey(Input.Key, Input.bPressed);
            }
        });

        (void)Window.OnMouseMove.AddLambda([&Game, &Window](FWindow*, const FMouseMoveInput& Input)
        {
            const FFieldViewport View = ComputeFieldViewport(Window.GetExtent());
            Game.OnMouseMoved((Input.X - View.OriginPixels.x) / Math::Max(View.UnitsToPixels, 0.0001f));
        });

        (void)Window.OnMouseButton.AddLambda([&Game](FWindow*, const FMouseButtonInput& Input)
        {
            if (Input.bPressed)
            {
                Game.OnKey(EKey::Space, true);
            }
        });
    }

    float MusicIntensityFor(const FGameState& State, bool bPaused)
    {
        if (bPaused)
        {
            return 0.12f;
        }

        switch (State.Phase)
        {
        case EPhase::Title:      return 0.30f;
        case EPhase::GameOver:   return 0.10f;
        case EPhase::LifeLost:   return 0.22f;
        case EPhase::LevelClear: return 0.85f;
        default:                 return 0.55f + Math::Min(float(State.Combo) * 0.04f, 0.45f);
        }
    }

    void PumpSound(FGame& Game, FSoundEngine& Sound)
    {
        TVector<FSoundRequest>& Pending = Game.GetPendingSounds();
        for (const FSoundRequest& Request : Pending)
        {
            Sound.Post(Request);
        }
        Pending.clear();

        const FGameState& State = Game.GetState();
        Sound.SetMusic(true, MusicIntensityFor(State, Game.IsPaused()), State.Level);

        const float Muffle = Game.IsPaused() ? 0.18f : (State.SlowTimer > 0.0f ? 0.30f : 1.0f);
        Sound.SetMusicFilter(Muffle * (1.0f - State.Danger * 0.25f));
    }

    void RunFrameLoop(FWindow& Window, RHI::FSwapchainTarget& Target, FGame& Game, FRenderer& Renderer, FSoundEngine& Sound)
    {
        double Previous = PlatformTime::Seconds();
        float RealTime = 0.0f;

        for (uint32 FrameSlot = 0; !Window.ShouldClose() && !Game.WantsQuit();
             FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
        {
            RHI::Core::BeginFrame(FrameSlot);

            Window.ProcessMessages();
            Target.Resize(Window.GetExtent());

            const double Now = PlatformTime::Seconds();
            const float Delta = float(Now - Previous);
            Previous = Now;
            RealTime += Delta;

            FFrameStats& Stats = Game.GetStats();
            Stats.FrameMilliseconds = Delta * 1000.0f;
            Stats.WorstMilliseconds = Math::Max(Stats.WorstMilliseconds * 0.995f, Stats.FrameMilliseconds);

            Game.Advance(Delta);
            PumpSound(Game, Sound);

            const FUIntVector2 Extent = Target.GetExtent();
            Renderer.EnsureTargets(Extent);

            const RHI::FTextureH SwapImage = Target.Acquire();
            if (!RHI::IsValid(SwapImage))
            {
                continue;
            }

            const RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
            Target.BarrierToRender(CL);

            Renderer.Render(CL, SwapImage, Target.GetExtent(), Game, RealTime);

            Target.Present(CL);
        }
    }
}

int main(int ArgC, char** ArgV)
{
    Memory::Initialize();

    FApplicationGlobalState GlobalState("Breakout Main");
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    FWindow Window(FWindowSpecs{ .Title = "Lumina Breakout", .Extent = { 1600, 900 } });

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });
    RHI::Core::Initialize();

    FSpirVShaderCompiler ShaderCompiler;
    GShaderCompiler = &ShaderCompiler;

    RHI::FSwapchainTarget Target;
    Target.Initialize(RHI::CreateSurface(Window.GetWindow()), Window.GetExtent());

    FGame Game;
    Game.Initialize();

    BindInput(Window, Game);

    FSoundEngine Sound;
    Sound.Initialize();

    FRenderer Renderer;
    const bool bReady = Renderer.Initialize(Target.GetFormat());

    if (bReady)
    {
        RunFrameLoop(Window, Target, Game, Renderer, Sound);
    }
    else
    {
        LOG_ERROR("Breakout: renderer initialization failed.");
    }

    Sound.Shutdown();

    RHI::WaitDeviceIdle();

    Renderer.Shutdown();
    Target.Shutdown();

    GShaderCompiler = nullptr;
    RHI::Core::Shutdown();
    RHI::FreeDevice();

    Task::Shutdown();
    GCommandLine = nullptr;

    return bReady ? 0 : 1;
}
