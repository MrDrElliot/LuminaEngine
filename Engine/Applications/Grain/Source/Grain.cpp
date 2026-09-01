#include "Render/Renderer.h"
#include "World/Camera.h"
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
        bool  bForward = false;
        bool  bBack    = false;
        bool  bLeft    = false;
        bool  bRight   = false;
        bool  bUp      = false;
        bool  bDown    = false;
        bool  bFast    = false;
        bool  bQuit    = false;
        bool  bLooking = false;
        bool  bDestroy = false;
        bool  bToggleTemporal = false;
        bool  bToggleFilter = false;
        float LastX    = 0.0f;
        float LastY    = 0.0f;
        float DeltaX   = 0.0f;
        float DeltaY   = 0.0f;
        bool  bHasLast = false;
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
            case EKey::Space:      Input.bUp      = Key.bPressed; break;
            case EKey::LeftControl:Input.bDown    = Key.bPressed; break;
            case EKey::LeftShift:  Input.bFast    = Key.bPressed; break;
            case EKey::Escape:     Input.bQuit    = Key.bPressed; break;
            case EKey::T:          Input.bToggleTemporal = Input.bToggleTemporal || Key.bPressed; break;
            case EKey::F:          Input.bToggleFilter = Input.bToggleFilter || Key.bPressed; break;
            default: break;
            }
        });

        (void)Window.OnMouseButton.AddLambda([&Input](FWindow*, const FMouseButtonInput& Button)
        {
            // Look moved to the right button so the left one can dig.
            if (Button.Button == EMouseKey::ButtonRight)
            {
                Input.bLooking = Button.bPressed;
                Input.bHasLast = false;
            }
            else if (Button.Button == EMouseKey::ButtonLeft && Button.bPressed)
            {
                Input.bDestroy = true;
            }
        });

        (void)Window.OnMouseMove.AddLambda([&Input](FWindow*, const FMouseMoveInput& Move)
        {
            const float X = float(Move.X);
            const float Y = float(Move.Y);

            if (Input.bLooking && Input.bHasLast)
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
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    FWindow Window(FWindowSpecs{ .Title = "Grain", .Extent = { 1600, 900 } });

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });

    // The default caps at the refresh rate, which hides how much headroom the frame actually has.
    if (ParsedCommandLine.Has("unlocked"))
    {
        RHI::SetPresentMode(EPresentMode::Mailbox);
    }
    RHI::Core::Initialize();

    FSpirVShaderCompiler ShaderCompiler;
    GShaderCompiler = &ShaderCompiler;

    RHI::FSwapchainTarget Target;
    Target.Initialize(RHI::CreateSurface(Window.GetWindow()), Window.GetExtent());

    FVoxelWorld World;
    World.Generate(1337u);

    const bool bUploaded = World.Upload();

    FCamera Camera;
    {
        const float StartX = kWorldSizeX * 0.5f;
        const float StartZ = kWorldSizeZ * 0.5f;
        const float Ground = World.SampleHeight(StartX, StartZ);
        FVector3 Cave;
        if (ParsedCommandLine.Has("cave") && World.FindCavePosition(Cave))
        {
            Camera.SetPosition(Cave);
            LOG_INFO("Grain: cave camera at {:.2f} {:.2f} {:.2f}.", Cave.x, Cave.y, Cave.z);
        }
        else
        {
            const float Eye = ParsedCommandLine.Has("lowcam") ? 2.2f : 14.0f;
            Camera.SetPosition({ StartX, Ground + Eye, StartZ });
        }
        LOG_INFO("Grain: camera at {:.1f} m over ground {:.1f} m.", Ground + 14.0f, Ground);
    }

    FVoxelSim Sim;
    if (bUploaded && !ParsedCommandLine.Has("nosim"))
    {
        const FVector3 Eye = Camera.GetPosition();
        Sim.Initialize(World, { Eye.x, Eye.y - kSimExtent * 0.25f, Eye.z });
    }

    if (Sim.IsValid() && ParsedCommandLine.Has("simcam"))
    {
        const FVector3 Spring = Sim.GetSpringWorld();
        const FVector3 Down = Sim.GetDownhill();
        const float Range = ParsedCommandLine.Has("simclose") ? 5.0f : 13.0f;
        Camera.SetPosition({ Spring.x + Down.x * Range, Spring.y + Range * 0.23f, Spring.z + Down.z * Range });
        Camera.LookAt({ Spring.x + Down.x * (Range * 0.4f), Spring.y - 2.0f, Spring.z + Down.z * (Range * 0.4f) });
        LOG_INFO("Grain: spring at {:.1f} {:.1f} {:.1f}, downhill {:.2f} {:.2f}, camera {:.1f} {:.1f} {:.1f}.",
            Spring.x, Spring.y, Spring.z, Down.x, Down.z,
            Camera.GetPosition().x, Camera.GetPosition().y, Camera.GetPosition().z);
    }

    FInput Input;
    BindInput(Window, Input);

    FRenderer Renderer;
    Renderer.SetDebugMode(ParsedCommandLine.Has("debugmat") ? 1u
                        : ParsedCommandLine.Has("debugnormal") ? 2u : 0u);
    Renderer.SetFilter(!ParsedCommandLine.Has("nofilter"));
    if (ParsedCommandLine.Has("gputimes")) { Renderer.EnableGpuTimers(); }
    Renderer.SetTemporal(!ParsedCommandLine.Has("notemporal"));
    const bool bReady = bUploaded && Renderer.Initialize(Target.GetFormat());

    if (!bReady)
    {
        LOG_ERROR("Grain: renderer initialization failed.");
    }

    double Previous = PlatformTime::Seconds();
    const double Started = Previous;
    uint32 Frames = 0;
    const uint32 CaptureFrame = uint32(ParsedCommandLine.GetInt("frames").value_or(48));
    uint32 Timed = 0;
    float Rendered = 0.0f;

    for (uint32 FrameSlot = 0; bReady && !Window.ShouldClose() && !Input.bQuit;
         FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
    {
        RHI::Core::BeginFrame(FrameSlot);

        Window.ProcessMessages();
        Target.Resize(Window.GetExtent());

        const double Now = PlatformTime::Seconds();
        const float Delta = Math::Min(float(Now - Previous), 0.1f);
        Previous = Now;

        const float LookX = Input.DeltaX;
        const float LookY = Input.DeltaY;
        Camera.Look(LookX, LookY);
        Input.DeltaX = 0.0f;
        Input.DeltaY = 0.0f;

        const FVector3 Local
        {
            float(Input.bRight ? 1 : 0) - float(Input.bLeft ? 1 : 0),
            float(Input.bUp ? 1 : 0) - float(Input.bDown ? 1 : 0),
            float(Input.bForward ? 1 : 0) - float(Input.bBack ? 1 : 0),
        };
        const bool bMoved = Local.x != 0.0f || Local.y != 0.0f || Local.z != 0.0f
                         || LookX != 0.0f || LookY != 0.0f;
        Camera.Move(Local, Delta, Input.bFast);

        if (ParsedCommandLine.Has("exitafter") && Now - Started > 6.0)
        {
            break;
        }

        if (Input.bToggleTemporal)
        {
            Input.bToggleTemporal = false;
            Renderer.SetTemporal(!Renderer.IsTemporalEnabled());
            LOG_INFO("Grain: temporal accumulation {}.", Renderer.IsTemporalEnabled() ? "on" : "off");
        }

        if (Input.bToggleFilter)
        {
            Input.bToggleFilter = false;
            Renderer.SetFilter(!Renderer.IsFilterEnabled());
            LOG_INFO("Grain: indirect filter {}.", Renderer.IsFilterEnabled() ? "on" : "off");
        }

        if (Input.bDestroy)
        {
            Input.bDestroy = false;
            Renderer.RequestDestroy(1.35f);
        }

        // A scripted dig, so a headless capture can show craters without a human at the mouse.
        if (ParsedCommandLine.Has("dig"))
        {
            if (Frames == 10)
            {
                Camera.Look(0.0f, 300.0f);
            }
            else if (Frames > 14 && Frames < 150 && (Frames % 4) == 0
                  && !ParsedCommandLine.Has("nodestroy"))
            {
                Renderer.RequestDestroy(2.4f);
                Camera.Look(7.0f, 0.0f);
            }
        }

        Renderer.EnsureTargets(Target.GetExtent());

        const RHI::FTextureH SwapImage = Target.Acquire();
        if (!RHI::IsValid(SwapImage))
        {
            continue;
        }

        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        Target.BarrierToRender(CL);

        Renderer.Render(CL, SwapImage, Target.GetExtent(), World, Sim, Camera, float(Now - Started), bMoved);

        Target.Present(CL);

        ++Frames;
        if (Frames > 8)
        {
            Rendered += Delta;
            ++Timed;
        }

        if (ParsedCommandLine.Has("screenshot") && Frames == CaptureFrame)
        {
            Renderer.CaptureToFile(Target.GetExtent(), "Grain.png");
            break;
        }
    }

    if (Timed > 0)
    {
        LOG_INFO("Grain: {:.2f} ms per frame over {} frames.", Rendered * 1000.0f / float(Timed), Timed);
    }

    RHI::WaitDeviceIdle();

    Renderer.ReportGpuTimers();
    Renderer.Shutdown();
    Sim.Release();
    World.Release();
    Target.Shutdown();

    GShaderCompiler = nullptr;
    RHI::Core::Shutdown();
    RHI::FreeDevice();

    Task::Shutdown();
    GCommandLine = nullptr;

    return bReady ? 0 : 1;
}
