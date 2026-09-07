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
using namespace Umbral;

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
            const FUIntVector2 Extent = Window.GetExtent();
            const float Scale = kViewHeight / float(Math::Max(Extent.y, 1u));
            Game.OnMouseMoved({ Input.X * Scale, Input.Y * Scale });
        });

        (void)Window.OnMouseButton.AddLambda([&Game](FWindow*, const FMouseButtonInput& Input)
        {
            if (Input.bPressed)
            {
                Game.OnMouseClick();
            }
        });
    }

    float MusicIntensityFor(const FRunState& Run, bool bPaused)
    {
        if (bPaused)
        {
            return 0.10f;
        }

        switch (Run.Phase)
        {
        case EPhase::Title:   return 0.24f;
        case EPhase::Dead:    return 0.08f;
        case EPhase::LevelUp: return 0.30f;
        default:              return Math::Min(0.42f + Run.Elapsed / 260.0f, 1.0f);
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

        const FRunState& Run = Game.GetRun();
        Sound.SetMusic(true, MusicIntensityFor(Run, Game.IsPaused()), int32(Run.Elapsed / 45.0f));

        const float Muffle = Game.IsPaused() || Run.Phase == EPhase::LevelUp ? 0.20f : 1.0f;
        Sound.SetMusicFilter(Muffle * (1.0f - Run.Danger * 0.35f));
    }

    void RunFrameLoop(FWindow& Window, RHI::FSwapchainTarget& Target, FGame& Game, FRenderer& Renderer,
                      FSoundEngine& Sound)
    {
        double Previous = PlatformTime::Seconds();
        float RealTime = 0.0f;

        for (uint32 FrameSlot = 0; !Window.ShouldClose() && !Game.WantsQuit();
             FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
        {
            RHI::BeginFrame(FrameSlot);

            Window.ProcessMessages();
            Target.Resize(Window.GetExtent());

            const double Now = PlatformTime::Seconds();
            const float Delta = float(Now - Previous);
            Previous = Now;
            RealTime += Delta;

            FFrameStats& Stats = Game.GetStats();
            Stats.FrameMilliseconds = Delta * 1000.0f;
            Stats.WorstMilliseconds = Math::Max(Stats.WorstMilliseconds * 0.995f, Stats.FrameMilliseconds);

            const FUIntVector2 ViewExtent = Target.GetExtent();
            Game.SetViewSize({ kViewHeight * float(ViewExtent.x) / float(Math::Max(ViewExtent.y, 1u)), kViewHeight });

            Game.Advance(Delta);
            PumpSound(Game, Sound);

            Renderer.EnsureTargets(Target.GetExtent());

            const RHI::FTextureH SwapImage = Target.Acquire();
            if (!RHI::IsValid(SwapImage))
            {
                continue;
            }

            const RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
            Target.BarrierToRender(CL);

            Renderer.Render(CL, SwapImage, Target.GetExtent(), Game, RealTime);

            Target.Present(CL);
        }
    }
}

int main(int ArgC, char** ArgV)
{
    Memory::Initialize();

    FApplicationGlobalState GlobalState("Umbral Main");
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    FWindow Window(FWindowSpecs{ .Title = "Umbral", .Extent = { 1600, 900 } });

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });

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
        LOG_ERROR("Umbral: renderer initialization failed.");
    }

    Sound.Shutdown();

    RHI::WaitDeviceIdle();

    Renderer.Shutdown();
    Target.Shutdown();

    GShaderCompiler = nullptr;
    RHI::FreeDevice();

    Task::Shutdown();
    GCommandLine = nullptr;

    return bReady ? 0 : 1;
}
