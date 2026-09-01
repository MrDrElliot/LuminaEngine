#include "RuntimePCH.h"
#include "Application.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Engine/Engine.h"
#include "Core/Module/ModuleManager.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowTypes.h"
#include "Events/Event.h"
#include "FileSystem/FileSystem.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"
#include "Paths/Paths.h"
#include "Log/Log.h"
#include "Renderer/RenderManager.h"
#include "Core/Diagnostics/BenchmarkRun.h"

namespace Lumina
{
    RUNTIME_API FApplication* GApp;

    FApplication::FApplication() = default;
    FApplication::~FApplication() = default;

    int32 FApplication::Run([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
    {
        LUMINA_PROFILE_SCOPE();

        ASSERT(GEngine);

        LOG_TRACE("Initializing Lumina");

        PreInitStartup();

        // A headless dedicated server has no window, no input viewport and no rendering surface.
        if (!GIsHeadless)
        {
            CreateApplicationWindow();
        }

        EventProcessor.RegisterEventHandler(&FInputViewportRegistry::Get(), (int32)EInputLayer::Viewport);

        #if !WITH_EDITOR
        if (!GIsHeadless)
        {
            PrimaryViewport = MakeUnique<FInputViewport>();
            const FUIntVector2 WinExtent = MainWindow->GetExtent();
            PrimaryViewport->SetWindowRect(0, 0, int(WinExtent.x), int(WinExtent.y));
            PrimaryViewport->SetRenderTargetSize(WinExtent.x, WinExtent.y);
            PrimaryViewport->SetHovered(true);
            PrimaryViewport->SetFocused(true);

            FInputViewportRegistry::Get().Register(PrimaryViewport.get());
            FInputViewportRegistry::Get().SetActiveViewport(PrimaryViewport.get());
            FInputViewportRegistry::Get().SetHoveredViewport(PrimaryViewport.get());
            FInputViewportRegistry::Get().SetFocusedViewport(PrimaryViewport.get());
        }
        #endif

        GEngine->Init();

        Benchmark::Start();

        bool bEngineWantsExit = false;
        while(!bEngineWantsExit)
        {
            LUMINA_PROFILE_FRAME();

            GEngine->MarkLoopStart();

            if (!GIsHeadless)
            {
                // Ahead of the event pump, so the mouse position this frame acts on is sampled after the
                // block rather than before it. Waiting later costs a whole frame of input latency.
                if (FRenderManager* RenderManager = TryRender())
                {
                    RenderManager->WaitForFrameSlot();
                }

                MainWindow->ProcessMessages();
            }

            bool bApplicationWantsExit = ShouldExit();

            // Evaluate input actions from the events just pumped, before anything reads them this frame.
            if (!GIsHeadless)
            {
                FInputViewportRegistry::Get().BeginFrame(GEngine->GetDeltaTime());
            }

            bEngineWantsExit = !GEngine->Update(bApplicationWantsExit);

            // A timed run ends itself, so the loop exits on the frame count rather than on a kill.
            if (Benchmark::IsActive() && !Benchmark::Tick(GEngine->GetDeltaTime()))
            {
                bEngineWantsExit = true;
            }

            if (!GIsHeadless)
            {
                FInputViewportRegistry::Get().EndFrame(GEngine->GetDeltaTime());
            }
        }

        LOG_TRACE("Shutting down Lumina");

        GEngine->Shutdown();

        if (PrimaryViewport)
        {
            FInputViewportRegistry::Get().Unregister(PrimaryViewport.get());
            PrimaryViewport.reset();
        }

        Shutdown();

        return 0;
    }

    void FApplication::Shutdown()
    {

    }

    void FApplication::WindowResized(FWindow* Window, const FUIntVector2& Extent)
    {
        if (!Window->IsWindowMinimized())
        {
            GEngine->SetEngineViewportSize(Extent);

            if (PrimaryViewport)
            {
                PrimaryViewport->SetWindowRect(0, 0, int(Extent.x), int(Extent.y));
                PrimaryViewport->SetRenderTargetSize(Extent.x, Extent.y);
            }
        }
    }

    void FApplication::RequestExit()
    {
        if (GApp != nullptr)
        {
            GApp->bExitRequested = true;
        }
    }

    void FApplication::CancelExit()
    {
        if (GApp == nullptr)
        {
            return;
        }

        GApp->bExitRequested = false;
        if (GApp->MainWindow)
        {
            GApp->MainWindow->CancelClose();
        }
    }

    void FApplication::PreInitStartup()
    {
        InitializeCObjectSystem();

        Paths::InitializePaths();
    }

    bool FApplication::CreateApplicationWindow()
    {
        (void)FWindow::OnWindowResized.AddMember(this, &FApplication::WindowResized);

        // The editor draws its own title bar, so it asks for a window without one.
        MainWindow = new FWindow(FWindowSpecs{ .bShowTitlebar = !WITH_EDITOR });

        (void)MainWindow->OnKey.AddMember(this, &FApplication::ForwardKey);
        (void)MainWindow->OnMouseButton.AddMember(this, &FApplication::ForwardMouseButton);
        (void)MainWindow->OnMouseMove.AddMember(this, &FApplication::ForwardMouseMove);
        (void)MainWindow->OnScroll.AddMember(this, &FApplication::ForwardScroll);
        (void)MainWindow->OnFileDrop.AddMember(this, &FApplication::ForwardFileDrop);
        (void)MainWindow->OnCloseRequested.AddMember(this, &FApplication::ForwardClose);

        Windowing::SetPrimaryWindowHandle(MainWindow);

        return true;
    }

    void FApplication::ForwardKey(FWindow* /*Window*/, const FKeyInput& Input)
    {
        if (Input.bPressed)
        {
            EventProcessor.Dispatch<FKeyPressedEvent>(Input.Key, Input.bCtrl, Input.bShift, Input.bAlt,
                Input.bSuper, Input.bRepeat);
        }
        else
        {
            EventProcessor.Dispatch<FKeyReleasedEvent>(Input.Key, Input.bCtrl, Input.bShift, Input.bAlt, Input.bSuper);
        }
    }

    void FApplication::ForwardMouseButton(FWindow* /*Window*/, const FMouseButtonInput& Input)
    {
        if (Input.bPressed)
        {
            EventProcessor.Dispatch<FMouseButtonPressedEvent>(Input.Button, Input.X, Input.Y);
        }
        else
        {
            EventProcessor.Dispatch<FMouseButtonReleasedEvent>(Input.Button, Input.X, Input.Y);
        }
    }

    void FApplication::ForwardMouseMove(FWindow* /*Window*/, const FMouseMoveInput& Input)
    {
        EventProcessor.Dispatch<FMouseMovedEvent>(Input.X, Input.Y, Input.DeltaX, Input.DeltaY);
    }

    void FApplication::ForwardScroll(FWindow* /*Window*/, const FMouseScrollInput& Input)
    {
        EventProcessor.Dispatch<FMouseScrolledEvent>(EMouseKey::Scroll, Input.Delta);
    }

    void FApplication::ForwardFileDrop(FWindow* /*Window*/, const TVector<FFixedString>& Paths,
        float MouseX, float MouseY)
    {
        EventProcessor.Dispatch<FFileDropEvent>(Paths, MouseX, MouseY);
    }

    void FApplication::ForwardClose(FWindow* /*Window*/)
    {
        RequestExit();
    }

    bool FApplication::ShouldExit() const
    {
        if (GIsHeadless)
        {
            return bExitRequested;
        }
        return MainWindow->ShouldClose() || bExitRequested;
    }
}
