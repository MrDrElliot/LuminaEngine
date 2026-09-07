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
        case EPhase::Draft:      return 0.35f;
        default:
        {
            const float Base = State.bBossAlive ? 0.72f : 0.55f;
            const float Heat = Math::Min(float(State.Combo) * 0.04f, 0.45f) + State.Progress * 0.15f;
            return Base + Heat + (State.IsFever() || State.IsVault() ? 0.2f : 0.0f);
        }
        }
    }

    EMusicMood MusicMoodFor(const FGameState& State)
    {
        switch (State.Phase)
        {
        case EPhase::Title:      return EMusicMood::Menu;
        case EPhase::GameOver:   return EMusicMood::Loss;
        case EPhase::LifeLost:   return EMusicMood::Loss;
        case EPhase::LevelClear: return EMusicMood::Clear;
        case EPhase::Draft:      return EMusicMood::Draft;
        default:
            if (State.IsFever())
            {
                return EMusicMood::Fever;
            }
            if (State.VaultGlow > 0.5f)
            {
                return EMusicMood::Vault;
            }
            return State.bBossAlive ? EMusicMood::Boss : EMusicMood::Play;
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
        Sound.SetMusic(true, MusicIntensityFor(State, Game.IsPaused()), State.Level, MusicMoodFor(State));
        Sound.SetMuted(Game.IsMuted());

        const float Muffle = Game.IsPaused() ? 0.18f : (State.SlowTimer > 0.0f ? 0.30f : (State.BlindTimer > 0.0f ? 0.45f : 1.0f));
        const float Frost = State.FreezeTimer > 0.0f ? 0.7f : 1.0f;
        Sound.SetMusicFilter(Muffle * Frost * (1.0f - State.Danger * 0.25f));
    }

    // Drives every mode headless with a scripted paddle, clearing stages by force so bosses and drafts all get exercised.
    int RunSoak()
    {
        FGame Game;
        Game.Initialize();

        int32 Failures = 0;
        const int32 Rounds = GCommandLine->Has("soakrounds") ? 4 : 1;
        for (int32 Round = 0; Round < Rounds; ++Round)
        for (int32 ModeIndex = 0; ModeIndex < int32(EGameMode::Count); ++ModeIndex)
        {
            for (int32 Guard = 0; int32(Game.GetState().MenuCursor) != ModeIndex && Guard < 16; ++Guard)
            {
                Game.OnKey(EKey::Right, true);
                Game.OnKey(EKey::Right, false);
                Game.Advance(1.0f / 60.0f);
            }
            for (int32 Guard = 0; Game.GetState().Phase == EPhase::Title && Guard < 30; ++Guard)
            {
                Game.OnKey(EKey::Space, true);
                Game.Advance(1.0f / 60.0f);
            }

            int32 MaxLevel = 0;
            int32 Drafts = 0;
            int32 Bosses = 0;
            int32 LastBossLevel = 0;
            int32 Frame = 0;

            for (; Frame < 12000; ++Frame)
            {
                const FGameState& State = Game.GetState();
                ECS::FRegistry& Registry = Game.GetRegistry();
                MaxLevel = Math::Max(MaxLevel, State.Level);

                if (State.Phase == EPhase::GameOver && State.PhaseTimer > 1.0f)
                {
                    LOG_INFO("  run ended by game over at frame {} level {} lives {}", Frame, State.Level, State.Lives);
                    Game.OnKey(EKey::Space, true);
                    Game.Advance(1.0f / 60.0f);
                    break;
                }

                if (State.Phase == EPhase::Title)
                {
                    LOG_INFO("  run ended by title at frame {} level {}", Frame, State.Level);
                    break;
                }

                float TargetX = kFieldWidth * 0.5f;
                float LowestY = -1.0f;
                for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
                {
                    if (Body.Position.y > LowestY)
                    {
                        LowestY = Body.Position.y;
                        TargetX = Body.Position.x;
                    }
                }
                Game.OnMouseMoved(TargetX + Math::Sin(float(Frame) * 0.05f) * 12.0f);

                if (State.IsCoop())
                {
                    float SecondX = kFieldWidth * 0.5f;
                    for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
                    {
                        if (Paddle.PlayerIndex == 1)
                        {
                            SecondX = Body.Position.x;
                        }
                    }
                    const bool bGoRight = TargetX > SecondX + 20.0f;
                    const bool bGoLeft = TargetX < SecondX - 20.0f;
                    Game.OnKey(EKey::Right, bGoRight);
                    Game.OnKey(EKey::Left, bGoLeft);
                }

                if (State.bBossAlive && State.Level != LastBossLevel)
                {
                    LastBossLevel = State.Level;
                    ++Bosses;
                }

                if (State.Phase == EPhase::Serve && Frame % 30 == 0)
                {
                    Game.OnKey(EKey::Space, true);
                }
                if (State.Phase == EPhase::Playing && Frame % 23 == 0)
                {
                    Game.OnKey(EKey::Space, true);
                }
                if (State.Phase == EPhase::Draft && State.PhaseTimer > 0.5f)
                {
                    ++Drafts;
                    Game.OnKey(Frame % 3 == 0 ? EKey::D1 : (Frame % 3 == 1 ? EKey::D2 : EKey::Space), true);
                }

                if (State.Phase == EPhase::Playing && Frame % 450 == 300)
                {
                    TVector<ECS::FEntity> Doomed;
                    for (const ECS::FEntity Entity : Registry.View<FBrick>()) { Doomed.push_back(Entity); }
                    for (const ECS::FEntity Entity : Registry.View<FBoss>()) { Doomed.push_back(Entity); }
                    for (const ECS::FEntity Entity : Doomed) { Registry.Destroy(Entity); }
                    Registry.GetSingleton<FGameState>().BricksAlive = 0;
                }

                Game.Advance(Frame % 5 == 0 ? 0.05f : 1.0f / 60.0f);
                Game.GetPendingSounds().clear();

            }

            const FGameState& Final = Game.GetState();
            const bool bOk = Final.Score > 0 && Drafts >= 1 && (MaxLevel >= 6 || Final.Phase == EPhase::Title);
            Failures += bOk ? 0 : 1;
            LOG_INFO("Soak {}: frames {} maxLevel {} drafts {} bosses {} score {} entities {} {}",
                GameModeName(Final.Mode), Frame, MaxLevel, Drafts, Bosses, Final.Score,
                Game.GetStats().Entities, bOk ? "OK" : "SHORT");

            if (Final.Phase != EPhase::Title)
            {
                Game.OnKey(EKey::T, true);
                Game.Advance(1.0f / 60.0f);
            }
        }

        LOG_INFO("Soak finished with {} failures", Failures);
        return Failures == 0 ? 0 : 1;
    }

    void RunFrameLoop(FWindow& Window, RHI::FSwapchainTarget& Target, FGame& Game, FRenderer& Renderer, FSoundEngine& Sound)
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

    FApplicationGlobalState GlobalState("Breakout Main");
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    if (ParsedCommandLine.Has("soak"))
    {
        const int Result = RunSoak();
        Task::Shutdown();
        GCommandLine = nullptr;
        return Result;
    }

    FWindow Window(FWindowSpecs{ .Title = "Lumina Breakout", .Extent = { 1600, 900 } });

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
        LOG_ERROR("Breakout: renderer initialization failed.");
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
