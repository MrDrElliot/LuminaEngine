#include "Game/Game.h"
#include "Render/Hud.h"
#include "Render/Renderer.h"
#include "World/Camera.h"
#include "World/Sky.h"
#include "World/VoxelSim.h"
#include "World/VoxelWorld.h"

#include "Core/Application/ApplicationGlobalState.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowTypes.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RHI.h"
#include "Renderer/PresentMode.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/SwapchainTarget.h"
#include "TaskSystem/TaskSystem.h"

using namespace Lumina;
using namespace Grain;

namespace
{
    struct FInput
    {
        bool bForward = false;
        bool bBack    = false;
        bool bLeft    = false;
        bool bRight   = false;
        bool bJump    = false;
        bool bSprint  = false;
        bool bInteract = false;
        bool bAttack  = false;
        bool bCast    = false;
        bool bQuit    = false;

        bool bToggleMouse = false;
        bool bToggleHud   = false;
        bool bToggleFilter = false;
        bool bToggleTemporal = false;
        bool bToggleAa = false;
        bool bToggleFree = false;

        float DeltaX = 0.0f;
        float DeltaY = 0.0f;
        float LastX  = 0.0f;
        float LastY  = 0.0f;
        bool  bHasLast = false;
        bool  bCaptured = true;
    };

    void BindInput(FWindow& Window, FInput& Input)
    {
        (void)Window.OnKey.AddLambda([&Input](FWindow*, const FKeyInput& Key)
        {
            if (Key.bRepeat)
            {
                return;
            }

            switch (Key.Key)
            {
            case EKey::W:          Input.bForward = Key.bPressed; break;
            case EKey::S:          Input.bBack    = Key.bPressed; break;
            case EKey::A:          Input.bLeft    = Key.bPressed; break;
            case EKey::D:          Input.bRight   = Key.bPressed; break;
            case EKey::Space:      Input.bJump    = Key.bPressed; break;
            case EKey::LeftShift:  Input.bSprint  = Key.bPressed; break;
            case EKey::E:          Input.bInteract = Key.bPressed; break;
            case EKey::Escape:     Input.bQuit    = Key.bPressed; break;
            case EKey::Tab:        Input.bToggleMouse = Input.bToggleMouse || Key.bPressed; break;
            case EKey::H:          Input.bToggleHud = Input.bToggleHud || Key.bPressed; break;
            case EKey::F:          Input.bToggleFilter = Input.bToggleFilter || Key.bPressed; break;
            case EKey::T:          Input.bToggleTemporal = Input.bToggleTemporal || Key.bPressed; break;
            case EKey::G:          Input.bToggleAa = Input.bToggleAa || Key.bPressed; break;
            case EKey::V:          Input.bToggleFree = Input.bToggleFree || Key.bPressed; break;
            default: break;
            }
        });

        (void)Window.OnMouseButton.AddLambda([&Input](FWindow*, const FMouseButtonInput& Button)
        {
            if (Button.Button == EMouseKey::ButtonLeft)
            {
                Input.bAttack = Button.bPressed;
            }
            else if (Button.Button == EMouseKey::ButtonRight)
            {
                Input.bCast = Button.bPressed;
            }
        });

        (void)Window.OnMouseMove.AddLambda([&Input](FWindow*, const FMouseMoveInput& Move)
        {
            const float X = float(Move.X);
            const float Y = float(Move.Y);

            if (Input.bHasLast)
            {
                Input.DeltaX += X - Input.LastX;
                Input.DeltaY += Y - Input.LastY;
            }

            Input.LastX = X;
            Input.LastY = Y;
            Input.bHasLast = true;
        });

        (void)Window.OnCloseRequested.AddLambda([&Input](FWindow*)
        {
            Input.bQuit = true;
        });
    }
}

int main(int ArgC, char** ArgV)
{
    Memory::Initialize();

    FApplicationGlobalState GlobalState("Grain Main");
    Jobs::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    Global::GCommandLine = &ParsedCommandLine;

    FWindow Window(FWindowSpecs{ .Title = "Grain: Ember Vale", .Extent = { 1600, 900 } });

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });

    // The default caps at the refresh rate, which hides how much headroom the frame actually has.
    if (ParsedCommandLine.Has("unlocked"))
    {
        RHI::SetPresentMode(EPresentMode::Mailbox);
    }

    FSpirVShaderCompiler ShaderCompiler;
    Global::GShaderCompiler = &ShaderCompiler;

    RHI::FSwapchainTarget Target;
    Target.Initialize(RHI::CreateSurface(Window.GetWindow()), Window.GetExtent());

    const uint32 Seed = uint32(ParsedCommandLine.GetInt("seed").Get(1337));

    FVoxelWorld World;
    World.Generate(Seed);

    const bool bUploaded = World.Upload();

    FGame Game;
    Game.Initialize(World, Seed);

    if (const auto Phase = ParsedCommandLine.GetInt("time"))
    {
        Game.SetTimeOfDay(float(*Phase) * 0.01f);
    }
    if (const auto Length = ParsedCommandLine.GetInt("daylength"))
    {
        Game.SetDayLength(float(*Length));
    }

    FVoxelSim Sim;
    if (bUploaded && !ParsedCommandLine.Has("nosim"))
    {
        const FVector3 Start = Game.GetPlayer().Body.Position;
        Sim.Initialize(World, { Start.x, Start.y - kSimExtent * 0.25f, Start.z });
    }

    FInput Input;
    BindInput(Window, Input);

    const bool bHeadless = ParsedCommandLine.Has("screenshot");
    const bool bRealStep = ParsedCommandLine.Has("realstep");
    if (bHeadless)
    {
        Input.bCaptured = false;
    }
    else
    {
        Window.SetCursorMode(ECursorMode::Disabled);
    }

    FRenderer Renderer;
    Renderer.SetDebugMode(ParsedCommandLine.Has("debugmat") ? 1u
                        : ParsedCommandLine.Has("debugnormal") ? 2u : 0u);
    Renderer.SetFilter(!ParsedCommandLine.Has("nofilter"));
    if (ParsedCommandLine.Has("gputimes")) { Renderer.EnableGpuTimers(); }
    Renderer.SetTemporal(!ParsedCommandLine.Has("notemporal"));
    Renderer.SetAntialiasing(!ParsedCommandLine.Has("noaa"));

    bool bReady = bUploaded && Renderer.Initialize(Target.GetFormat());

    if (bReady)
    {
        const FModelLibrary& Models = Game.GetModels();
        bReady = Renderer.UploadModels(
            TSpan<const uint32>(reinterpret_cast<const uint32*>(Models.Descs().GetData()),
                                Models.Descs().Num() * 4),
            TSpan<const uint32>(Models.PackedCells().GetData(), Models.PackedCells().Num()));
    }

    FHud Hud;
    if (bReady)
    {
        Hud.Initialize(Target.GetFormat());
        Hud.SetVisible(!ParsedCommandLine.Has("nohud"));
    }
    else
    {
        LOG_ERROR("Grain: renderer initialization failed.");
    }

    FCamera Camera;
    bool bFreeCamera = ParsedCommandLine.Has("freecam");

    double Previous = PlatformTime::Seconds();
    const double Started = Previous;
    uint32 Frames = 0;
    const uint32 CaptureFrame = uint32(ParsedCommandLine.GetInt("frames").Get(48));
    uint32 Timed = 0;
    float Rendered = 0.0f;

    TVector<FEntityGpu> Visible;
    Visible.Reserve(kMaxRenderEntities);

    for (uint32 FrameSlot = 0; bReady && !Window.ShouldClose() && !Input.bQuit;
         FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
    {
        RHI::BeginFrame(FrameSlot);

        Window.ProcessMessages();
        Target.Resize(Window.GetExtent());

        const double Now = PlatformTime::Seconds();
        const float Wall = Math::Min(float(Now - Previous), 0.1f);
        Previous = Now;

        // A capture advances on a fixed step, so the same frame number always shows the same moment.
        const float Delta = bHeadless && !bRealStep ? 1.0f / 60.0f : Wall;

        if (Input.bToggleMouse)
        {
            Input.bToggleMouse = false;
            Input.bCaptured = !Input.bCaptured;
            Input.bHasLast = false;
            Window.SetCursorMode(Input.bCaptured ? ECursorMode::Disabled : ECursorMode::Normal);
        }

        if (Input.bToggleHud)
        {
            Input.bToggleHud = false;
            Hud.SetVisible(!Hud.IsVisible());
        }

        if (Input.bToggleFilter)
        {
            Input.bToggleFilter = false;
            Renderer.SetFilter(!Renderer.IsFilterEnabled());
        }

        if (Input.bToggleTemporal)
        {
            Input.bToggleTemporal = false;
            Renderer.SetTemporal(!Renderer.IsTemporalEnabled());
        }

        if (Input.bToggleAa)
        {
            Input.bToggleAa = false;
            Renderer.SetAntialiasing(!Renderer.IsAntialiasingEnabled());
        }

        if (Input.bToggleFree)
        {
            Input.bToggleFree = false;
            bFreeCamera = !bFreeCamera;
            LOG_INFO("Grain: free camera {}.", bFreeCamera ? "on" : "off");
        }

        FGameInput GameInput;
        if (Input.bCaptured)
        {
            GameInput.LookX = Input.DeltaX * 0.0024f;
            GameInput.LookY = Input.DeltaY * 0.0024f;
        }
        Input.DeltaX = 0.0f;
        Input.DeltaY = 0.0f;

        GameInput.Move =
        {
            float(Input.bRight ? 1 : 0) - float(Input.bLeft ? 1 : 0),
            float(Input.bForward ? 1 : 0) - float(Input.bBack ? 1 : 0),
        };
        GameInput.bJump = Input.bJump;
        GameInput.bSprint = Input.bSprint;
        GameInput.bAttack = Input.bAttack;
        GameInput.bCast = Input.bCast;
        GameInput.bInteract = Input.bInteract;

        // A scripted walk, so a headless capture has something to look at without a hand on the keys.
        if (bHeadless)
        {
            // Walk out a little, then hold, turn and keep swinging so the capture shows a fight.
            GameInput.Move = Frames < 90 ? FVector2{ 0.0f, 1.0f } : FVector2{ 0.0f, 0.0f };
            GameInput.LookX = Frames < 90 ? 0.0f : 0.0034f;
            GameInput.bAttack = Frames > 30;
            GameInput.bCast = (Frames % 180) == 0;
        }

        Game.Update(Delta, GameInput, Renderer);

        if (bHeadless && (Frames % 240) == 0)
        {
            const FPlayerState& Report = Game.GetPlayer();
            LOG_INFO("Grain: t{:.1f}s hp {:.0f} lv {} shards {} felled {} husks {} wisps {} golems {} "
                     "shardsOnGround {} debris {} sparks {} grounded {} water {}",
                float(Frames) / 60.0f, Report.Health, Report.Level, Report.Shards, Report.Kills,
                Game.CountOf(EEntityKind::Husk), Game.CountOf(EEntityKind::Wisp),
                Game.CountOf(EEntityKind::Golem), Game.CountOf(EEntityKind::Shard),
                Game.CountOf(EEntityKind::Debris), Game.CountOf(EEntityKind::Spark),
                Report.Body.bGrounded, Report.Body.bInWater);
        }

        //~ The camera follows the player unless the free camera is flying the scene for a capture.

        if (bFreeCamera)
        {
            Camera.Look(GameInput.LookX * 420.0f, GameInput.LookY * 420.0f);
            Camera.Move({ GameInput.Move.x, 0.0f, GameInput.Move.y }, Delta, Input.bSprint);
        }
        else
        {
            Camera.SetPosition(Game.GetCameraPosition());
            Camera.SetOrientation(Game.GetPlayer().Yaw, Game.GetPlayer().Pitch);
        }

        //~ Everything the game owns that the ray marcher has to see.

        Visible.Reset();
        {
            const FVector3 Eye = Camera.GetPosition();

            const auto Push = [&Visible, &Game](const FVector3& Position, float Yaw, float Scale,
                                                EModel Model, const FVector3& Tint, float Emissive)
            {
                if (Visible.Num() >= kMaxRenderEntities)
                {
                    return;
                }

                const FVector3 Half = Game.GetModels().HalfExtentOf(Model, Scale);

                FEntityGpu Record;
                Record.Position = { Position.x / kVoxelSize, Position.y / kVoxelSize,
                                    Position.z / kVoxelSize };
                Record.Yaw = Yaw;
                Record.Half = { Half.x / kVoxelSize, Half.y / kVoxelSize, Half.z / kVoxelSize };
                Record.Model = uint32(Model);
                Record.Tint = Tint;
                Record.Emissive = Emissive;
                Visible.Add(Record);
            };

            const FPlayerState& Player = Game.GetPlayer();
            if (Player.bAlive && !bFreeCamera && !Game.IsFirstPerson())
            {
                const float Flash = Player.HurtTimer > 0.0f ? 1.0f + Player.HurtTimer * 3.0f : 1.0f;
                Push(Player.Body.Position, Player.Yaw - 1.5707963f, 1.0f, EModel::Delver,
                     { Flash, 1.0f, 1.0f }, 0.0f);
            }

            for (const FEntity& Entity : Game.GetEntities())
            {
                if (!Entity.bActive)
                {
                    continue;
                }

                // A far entity covers less than a pixel, so the per ray loop never has to see it.
                const FVector3 Rel { Entity.Body.Position.x - Eye.x,
                                     Entity.Body.Position.y - Eye.y,
                                     Entity.Body.Position.z - Eye.z };
                if (Rel.x * Rel.x + Rel.y * Rel.y + Rel.z * Rel.z > 120.0f * 120.0f)
                {
                    continue;
                }

                FVector3 Tint = Entity.Tint;
                if (Entity.HurtFlash > 0.0f)
                {
                    Tint = { Tint.x + Entity.HurtFlash * 2.5f,
                             Tint.y * (1.0f - Entity.HurtFlash * 0.6f),
                             Tint.z * (1.0f - Entity.HurtFlash * 0.6f) };
                }

                Push(Entity.Body.Position, Entity.Yaw - 1.5707963f, Entity.Scale, Entity.Model,
                     Tint, Entity.Emissive);
            }
        }

        Renderer.SetEntities(TSpan<const FEntityGpu>(Visible.GetData(), Visible.Num()));
        Renderer.SetSky(Game.GetSky());

        {
            FFrameTint Tint;
            const FPlayerState& Player = Game.GetPlayer();

            if (Player.HurtTimer > 0.0f)
            {
                const float Hurt = Player.HurtTimer / 0.45f;
                Tint.Color = { 1.0f, 1.0f - Hurt * 0.35f, 1.0f - Hurt * 0.40f };
                Tint.Saturation = 1.16f - Hurt * 0.45f;
            }

            if (!Player.bAlive)
            {
                Tint.Saturation = 0.25f;
                Tint.Color = { 1.0f, 0.72f, 0.68f };
            }

            Renderer.SetTint(Tint);
        }

        Renderer.EnsureTargets(Target.GetExtent());

        const RHI::FTextureH SwapImage = Target.Acquire();
        if (!RHI::IsValid(SwapImage))
        {
            continue;
        }

        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        Target.BarrierToRender(CL);

        Renderer.Render(CL, SwapImage, Target.GetExtent(), World, Sim, Camera,
                        float(Now - Started), Delta);

        Hud.Build(Game, Target.GetExtent(), float(Now - Started));
        Hud.Draw(CL, SwapImage, Target.GetExtent());

        Target.Present(CL);

        ++Frames;
        if (Frames > 8)
        {
            Rendered += Wall;
            ++Timed;
        }

        if (bHeadless && Frames == CaptureFrame)
        {
            Renderer.CaptureToFile(Target.GetExtent(), "Grain.png",
                [&Hud, &Target](RHI::FCmdListH Capture, RHI::FTextureH Image)
                {
                    Hud.Draw(Capture, Image, Target.GetExtent());
                });
            break;
        }
    }

    if (Timed > 0)
    {
        LOG_INFO("Grain: {:.2f} ms per frame over {} frames.", Rendered * 1000.0f / float(Timed), Timed);
    }

    RHI::WaitDeviceIdle();

    Renderer.ReportGpuTimers();
    Hud.Shutdown();
    Renderer.Shutdown();
    Sim.Release();
    World.Release();
    Target.Shutdown();

    Global::GShaderCompiler = nullptr;
    RHI::FreeDevice();

    Jobs::Shutdown();
    Global::GCommandLine = nullptr;

    return bReady ? 0 : 1;
}
