#include "RuntimePCH.h"
#include "GameDevUI.h"

#if USING(WITH_DEV_UI)

#include "Containers/StringFormat.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Module/ModuleManager.h"
#include "Events/Event.h"
#include "Input/InputViewport.h"
#include "Paths/Paths.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RenderManager.h"
#include "Scripting/EntityScript.h"
#include "Tools/Image/ImageWrite.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "World/World.h"
#include "World/WorldManager.h"

#include <imgui.h>

namespace Lumina
{
    namespace
    {
        constexpr EKey kToggleKey = EKey::GraveAccent;

        // Long enough for the startup map's first frames to present, so an -exec screenshot shows the game rather than a black window.
        constexpr int32 kExecStartDelayFrames = 90;

        bool IsInputEvent(FEvent& Event)
        {
            return Event.IsA<FKeyPressedEvent>() || Event.IsA<FKeyReleasedEvent>() || Event.IsA<FCharInputEvent>()
                || Event.IsA<FMouseMovedEvent>() || Event.IsA<FMouseButtonPressedEvent>()
                || Event.IsA<FMouseButtonReleasedEvent>() || Event.IsA<FMouseScrolledEvent>();
        }

        // Console commands take no arguments, so they reach the overlay through the engine that owns it.
        FGameDevUI* GetGameDevUI()
        {
            return GEngine != nullptr ? static_cast<FGameDevUI*>(GEngine->GetDevelopmentToolsUI()) : nullptr;
        }

        void ScreenshotCommand()
        {
            if (FGameDevUI* DevUI = GetGameDevUI())
            {
                DevUI->RequestScreenshot();
            }
        }

        void ToggleCommand()
        {
            if (FGameDevUI* DevUI = GetGameDevUI())
            {
                DevUI->SetOpen(!DevUI->IsOpen());
            }
        }

        void QuitCommand()
        {
            if (GEngine != nullptr)
            {
                GEngine->RequestExitGame();
            }
        }

        FString ScreenshotPath()
        {
            const FStringView ProjectPath = GEngine->GetProjectPath();
            const FString Folder = ProjectPath.IsEmpty()
                ? Paths::GetEngineDirectory() + "/Saved/Screenshots"
                : FString(*Paths::Combine(ProjectPath, "Saved", "Screenshots"));
            Paths::CreateDirectories(FStringView(*Folder, Folder.Len()));

            const PlatformTime::FDateTime Now = PlatformTime::LocalNow();
            return Lumina::Format("{}/Screenshot_{:04}{:02}{:02}_{:02}{:02}{:02}.png", Folder,
                Now.Year, Now.Month, Now.Day, Now.Hour, Now.Minute, Now.Second);
        }
    }

    void FGameDevUI::Initialize(const FUpdateContext& UpdateContext)
    {
        IImGuiRenderer* ImGuiRenderer = Render().GetImGuiRenderer();

        // Game modules link their own ImGui, so each one is pointed at this context before its first widget call.
        FModuleManager::Get().NotifyImGuiReady(ImGuiRenderer->GetImGuiContext(), ImGuiRenderer->GetImPlotContext());

        FConsoleRegistry::Get().RegisterCommand(FConsoleCommand("screenshot", "Saves the next frame to the project's Saved/Screenshots.", &ScreenshotCommand));
        FConsoleRegistry::Get().RegisterCommand(FConsoleCommand("quit", "Closes the game.", &QuitCommand));
        FConsoleRegistry::Get().RegisterCommand(FConsoleCommand("DevUI.Toggle", "Opens or closes the developer overlay, as the grave key does.", &ToggleCommand));

        if (GCommandLine != nullptr)
        {
            // Split on semicolons, as -exec="DebugDraw.Enabled false;screenshot" lists several lines.
            if (const TOptional<FFixedString> Exec = GCommandLine->Get("exec"); Exec.IsSet())
            {
                FStringView Remaining(*(*Exec), Exec->Len());
                while (!Remaining.IsEmpty())
                {
                    const int32 Split = Remaining.Find(';');
                    const FStringView Line = Split == Constants::kIndexNone ? Remaining : Remaining.Left(Split);
                    if (!Line.IsEmpty())
                    {
                        PendingExecLines.Emplace(Line.GetData(), Line.Len());
                    }
                    Remaining = Split == Constants::kIndexNone ? FStringView() : Remaining.Mid(Split + 1);
                }
            }
            SetOpen(GCommandLine->Has("devui"));
        }

        ApplyImGuiInputFlags();
    }

    void FGameDevUI::Deinitialize(const FUpdateContext& UpdateContext)
    {
        SetOpen(false);
    }

    void FGameDevUI::SetOpen(bool bInOpen)
    {
        if (bOpen == bInOpen)
        {
            return;
        }
        bOpen = bInOpen;
        bFocusConsoleNextFrame = bOpen;
        FInputViewportRegistry::Get().SetDevOverlayCapturing(bOpen);
        ApplyImGuiInputFlags();
    }

    void FGameDevUI::RequestScreenshot()
    {
        PendingScreenshotTicket = Render().RequestWindowCapture();
        bScreenshotPending = true;
    }

    void FGameDevUI::ApplyImGuiInputFlags()
    {
        constexpr ImGuiConfigFlags PassThroughFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        ImGuiIO& IO = ImGui::GetIO();
        IO.ConfigFlags = bOpen ? (IO.ConfigFlags & ~PassThroughFlags) : (IO.ConfigFlags | PassThroughFlags);
    }

    bool FGameDevUI::OnEvent(FEvent& Event)
    {
        if (Event.IsA<FKeyPressedEvent>())
        {
            const FKeyPressedEvent& Key = Event.As<FKeyPressedEvent>();
            if (Key.GetKeyCode() == kToggleKey)
            {
                if (!Key.IsRepeat())
                {
                    SetOpen(!bOpen);
                }
                return true;
            }
            if (bOpen && Key.GetKeyCode() == EKey::Escape)
            {
                SetOpen(false);
                return true;
            }
        }

        // ImGui reads the window itself, so swallowing these only keeps the game from acting on input meant for the overlay.
        return bOpen && IsInputEvent(Event);
    }

    void FGameDevUI::OnEndFrame(const FUpdateContext& UpdateContext)
    {
        FinishPendingScreenshot();
        RunPendingExecLines();

        if (bOpen)
        {
            Console.Draw(bFocusConsoleNextFrame, bKeepScriptWindowsVisible);
            bFocusConsoleNextFrame = false;
        }

        if (!bOpen && !bKeepScriptWindowsVisible)
        {
            return;
        }

        GWorldManager->ForEachWorld([](CWorld& World)
        {
            if (World.IsGameWorld() && !World.IsSuspended())
            {
                EntityScripts::DebugGui(World.GetEntityRegistry());
            }
        });
    }

    void FGameDevUI::RunPendingExecLines()
    {
        if (PendingExecLines.IsEmpty() || bScreenshotPending)
        {
            return;
        }

        bool bHasGameWorld = false;
        GWorldManager->ForEachWorld([&bHasGameWorld](CWorld& World)
        {
            bHasGameWorld = bHasGameWorld || (World.IsGameWorld() && !World.IsSuspended());
        });
        FramesWithGameWorld = bHasGameWorld ? FramesWithGameWorld + 1 : 0;
        if (FramesWithGameWorld < kExecStartDelayFrames)
        {
            return;
        }

        // One line a frame, so each sees the frame the line before it changed.
        const FString Line = Move(PendingExecLines[0]);
        PendingExecLines.RemoveAt(0);
        FConsoleRegistry::Get().ExecuteLine(Line, true);
    }

    void FGameDevUI::FinishPendingScreenshot()
    {
        FRenderManager::FWindowCapture Capture;
        if (!bScreenshotPending || !Render().TakeWindowCapture(PendingScreenshotTicket, Capture))
        {
            return;
        }
        bScreenshotPending = false;

        // The swapchain is BGRA and UI passes leave arbitrary alpha behind, which a PNG would show as holes.
        for (int32 Index = 0; Index + 3 < Capture.Pixels.Num(); Index += 4)
        {
            std::swap(Capture.Pixels[Index], Capture.Pixels[Index + 2]);
            Capture.Pixels[Index + 3] = 255;
        }

        const FString Path = ScreenshotPath();
        if (ImageWrite::WritePngFile(*Path, Capture.Width, Capture.Height, 4, Capture.Pixels.GetData(), Capture.Width * 4))
        {
            LOG_INFO("Saved screenshot ({}x{}) to {}", Capture.Width, Capture.Height, Path);
        }
        else
        {
            LOG_ERROR("Screenshot could not be written to {}", Path);
        }
    }
}

#endif
