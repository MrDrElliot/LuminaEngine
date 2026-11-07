#include "MCPEditorSessionTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolRegistry.h"
#include "Containers/Algorithm.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Threading/Thread.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowInput.h"
#include "FileSystem/FileSystem.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"
#include "Log/Log.h"
#include "MCPEditorModule.h"
#include "MCPTextMatch.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RenderManager.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Session/SessionOps.h"
#include "Tools/Screenshot/ScreenshotCapture.h"
#include "UI/Tools/EditorToolContext.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace MCPEditorSessionToolsInternal
    {
        constexpr const char* GNoWorldEditorSession = "No world editor is open.";

        void FillEditorPlayState(SPlayState& Out)
        {
            FString Error;
            SessionOps::FPlayState State;
            (void)SessionOps::GetPlayState(State, Error);

            Out.bPlaying = State.bPlaying;
            Out.bPaused  = State.bPaused;
            Out.World    = State.World;
            Out.bEjected = State.bEjected;

            const FInputViewportRegistry& Viewports = FInputViewportRegistry::Get();
            Out.bGameInputFocused = Viewports.IsGameInputFocused();
            switch (Viewports.GetActiveEffectiveMouseMode())
            {
            case EMouseMode::Normal:   Out.MouseMode = "Normal";   break;
            case EMouseMode::Hidden:   Out.MouseMode = "Hidden";   break;
            case EMouseMode::Captured: Out.MouseMode = "Captured"; break;
            }
            if (const FInputViewport* Active = Viewports.GetActiveViewport())
            {
                switch (Active->GetContext().GetInputMode())
                {
                case EInputMode::Game:      Out.InputMode = "Game";      break;
                case EInputMode::UI:        Out.InputMode = "UI";        break;
                case EInputMode::GameAndUI: Out.InputMode = "GameAndUI"; break;
                }
            }
        }

        const char* EditorLogLevelName(ELogLevel Level)
        {
            switch (Level)
            {
            case ELogLevel::Trace:    return "Trace";
            case ELogLevel::Debug:    return "Debug";
            case ELogLevel::Info:     return "Info";
            case ELogLevel::Warn:     return "Warn";
            case ELogLevel::Error:    return "Error";
            case ELogLevel::Critical: return "Critical";
            default:                  return "Off";
            }
        }

        bool ParseEditorLogLevel(const FString& Text, ELogLevel& OutLevel)
        {
            for (uint8 Index = 0; Index <= static_cast<uint8>(ELogLevel::Critical); ++Index)
            {
                const ELogLevel Level = static_cast<ELogLevel>(Index);
                if (EqualsTextFold(FStringView(EditorLogLevelName(Level)), FStringView(Text)))
                {
                    OutLevel = Level;
                    return true;
                }
            }

            return false;
        }

        // Injected input only reaches the viewport that owns input, which the editor moves only while its window is foreground.
        bool FocusEditorGameViewport(bool& bOutFocused)
        {
            return Agent::FGameThreadGate::Run([&bOutFocused]()
            {
                FInputViewportRegistry& Viewports = FInputViewportRegistry::Get();
                FString SceneError;
                CWorld* World = SessionOps::GetSceneWorld(SceneError);
                Viewports.TakeGameFocus(World != nullptr ? Viewports.FindViewportForWorld(World) : nullptr);
                bOutFocused = Viewports.GetActiveViewport() != nullptr;
            }, Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds()) == Agent::EGameThreadResult::Ran;
        }

        void FillEditorWindowState(FWindow& Window, SWindowState& Out)
        {
            const FUIntVector2 Extent = Window.GetExtent();
            Out.Width  = (int32)Extent.x;
            Out.Height = (int32)Extent.y;
            Window.GetWindowPosition(Out.X, Out.Y);
            Out.bMinimized = Window.IsWindowMinimized();
            Out.bMaximized = Window.IsWindowMaximized();
        }

        Agent::FToolResult StepEditorHistory(const SEditorHistoryParams& In, SEditorHistoryResult& Out)
        {
            const bool bUndo = In.Action == EEditorHistoryAction::Undo;
            FString SessionError;
            const FStringView Tab(In.Tab);
            if (Tab.IsEmpty() && !SessionOps::HasSceneEditor())
            {
                return Agent::FToolResult::Error(GNoWorldEditorSession);
            }

            if (!SessionOps::HasUndoTarget(Tab, SessionError))
            {
                return Agent::FToolResult::Error(SessionError);
            }

            if (Tab.IsEmpty() && SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(bUndo ? "Undo is blocked while playing in editor." : "Redo is blocked while playing in editor.");
            }

            for (int32 Step = 0; Step < Math::Max(In.Steps, 1) && (bUndo ? SessionOps::Undo(1, Tab) : SessionOps::Redo(1, Tab)) > 0; ++Step)
            {
                ++Out.Applied;
            }

            const SessionOps::FUndoState Undo = SessionOps::GetUndoState(Tab);
            Out.NextUndo = Undo.NextUndo;
            Out.NextRedo = Undo.NextRedo;

            if (Out.Applied == 0)
            {
                return Agent::FToolResult::Error(bUndo ? "Nothing to undo." : "Nothing to redo.");
            }

            const FString& Next = bUndo ? Out.NextUndo : Out.NextRedo;
            return Agent::FToolResult::Ok(Lumina::Format("{} {} step(s). Next {}: {}.", bUndo ? "Undid" : "Redid",
                Out.Applied, bUndo ? "undo" : "redo", Next.IsEmpty() ? FString("nothing") : Next));
        }

        Agent::FToolResult StartEditorPlay(const SEditorPlayParams& In, SPlayState& Out)
        {
            FString SessionError;
            if (In.Players > 0 || !In.NetMode.IsEmpty())
            {
                const FString Mode = In.NetMode.IsEmpty() ? FString("listen") : In.NetMode;
                const int32 Players = In.Players > 0 ? In.Players : 1;
                if (!SessionOps::SetPlayNetwork(Players, FStringView(*Mode, Mode.Len()), SessionError))
                {
                    return Agent::FToolResult::Error(SessionError);
                }
            }

            if (!SessionOps::StartPlay(SessionError))
            {
                FillEditorPlayState(Out);
                return Agent::FToolResult::Error("Already playing.");
            }

            LOG_INFO("[MCP] An agent started play-in-editor.");
            FillEditorPlayState(Out);
            return Agent::FToolResult::Ok("Playing.");
        }

        Agent::FToolResult ControlEditorPlay(EEditorPlayAction Action, SPlayState& Out)
        {
            if (!SessionOps::IsSimulating())
            {
                FillEditorPlayState(Out);
                return Agent::FToolResult::Error("Nothing is playing.");
            }

            FString SessionError;
            if (Action == EEditorPlayAction::Stop)
            {
                SessionOps::StopPlay(SessionError);
                LOG_INFO("[MCP] An agent stopped play-in-editor.");
                FillEditorPlayState(Out);
                return Agent::FToolResult::Ok("Stopped.");
            }

            const bool bPause = Action == EEditorPlayAction::Pause;
            SessionOps::SetPaused(bPause, SessionError);
            FillEditorPlayState(Out);
            return Agent::FToolResult::Ok(bPause ? "Paused." : "Resumed.");
        }

        Agent::FToolResult ListEditorTabs(SEditorTabsResult& Out)
        {
            SessionOps::ForEachTab([&Out](const SessionOps::FTabInfo& Tab)
            {
                STabInfo Info;
                Info.Name      = Tab.Name;
                Info.Id        = Tab.Id;
                Info.AssetGuid = Tab.AssetGuid;
                Info.bUnsaved  = Tab.bUnsaved;
                Info.bFocused  = Tab.bFocused;
                Info.bClosable = Tab.bClosable;
                Out.Tabs.Add(Move(Info));
            });

            FString Text;
            for (const STabInfo& Tab : Out.Tabs)
            {
                Text.Append(Lumina::Format("{}{}{}\n", Tab.Name, Tab.bFocused ? " (focused)" : "", Tab.bUnsaved ? " *" : ""));
            }

            return Agent::FToolResult::Ok(Text.IsEmpty() ? FString("No tabs are open.") : Text);
        }

        Agent::FToolResult FocusEditorTab(const SEditorTabsParams& In, SEditorTabsResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::FocusTab(FStringView(In.Tab), Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error + " Call editor.tabs List.");
            }

            return Agent::FToolResult::Ok(Lumina::Format("Focusing '{}'.", In.Tab));
        }

        Agent::FToolResult OpenEditorToolPanel(const SEditorTabsParams& In, SEditorTabsResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::OpenToolPanel(FStringView(In.Tab), Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Opening '{}'.", In.Tab));
        }

        Agent::FToolResult CloseEditorTab(const SEditorTabsParams& In, SEditorTabsResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::CloseTab(FStringView(In.Tab), In.bDiscardUnsaved, Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Closing '{}'.", In.Tab));
        }

        Agent::FToolResult FloatEditorTab(const SEditorTabsParams& In, SEditorTabsResult& Out)
        {
            if (In.Width <= 0 || In.Height <= 0)
            {
                return Agent::FToolResult::Error("Width and Height have to be positive.");
            }

            FString Error;
            Out.bDone = SessionOps::FloatTab(FStringView(In.Tab), FVector2((float)In.X, (float)In.Y),
                                             FVector2((float)In.Width, (float)In.Height), Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error);
            }
            return Agent::FToolResult::Ok(Lumina::Format("Floating {} at ({}, {}), {}x{}.", In.Tab, In.X, In.Y, In.Width, In.Height));
        }

        Agent::FToolResult TargetEditorTab(const SEditorTabsParams& In, SEditorTabsResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::SetSceneTarget(FStringView(In.Tab), Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error + " Call editor.tabs List.");
            }

            return Agent::FToolResult::Ok(In.Tab.IsEmpty()
                ? FString("Scene tools target the world editor.")
                : Lumina::Format("Scene tools target '{}'.", In.Tab));
        }

        Agent::FToolResult OpenEditorAsset(const FString& Asset, SEditorOpenResult& Out)
        {
            const TOptional<FGuid> Guid = Agent::ParseAssetGuid(FStringView(Asset));
            if (!Guid.IsSet())
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' is neither a GUID nor an asset path.", Asset));
            }

            FString Error;
            if (!SessionOps::OpenAsset(*Guid, Out.Tab, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Open in tab '{}'.", Out.Tab));
        }

        Agent::FToolResult OpenEditorFile(const FString& Path, SEditorOpenResult& Out)
        {
            IEditorToolContext* Tools = SessionOps::GetToolContext();
            if (Tools == nullptr)
            {
                return Agent::FToolResult::Error("The editor UI is not running.");
            }
            if (!VFS::Exists(FStringView(*Path, Path.Len())))
            {
                return Agent::FToolResult::Error(Lumina::Format("No file at '{}'.", Path));
            }
            Tools->OpenFileEditor(FStringView(*Path, Path.Len()));
            Out.Tab = Path;
            return Agent::FToolResult::Ok(Lumina::Format("Opened '{}'.", Path));
        }

        Agent::FToolResult OpenEditorLevel(const FString& Level, SEditorOpenResult& Out)
        {
            if (GEngine == nullptr)
            {
                return Agent::FToolResult::Error("The engine is not running.");
            }

            const FURL Url = FURL::Parse(FStringView(*Level, Level.Len()));
            Out.Map = Url.Map;
            GEngine->OpenLevel(Url);
            return Agent::FToolResult::Ok(Lumina::Format("Opening {}.", Url.Map));
        }

        Agent::FToolResult SendEditorText(FEditorInputState& State, const SEditorInputParams& In, SEditorInputResult& Out)
        {
            if (!FocusEditorGameViewport(Out.bGameInputFocused))
            {
                return Agent::FToolResult::Error("The game thread did not pick up the focus change in time.");
            }

            // ImGui releases the keyboard at the next frame start, so text sent this frame would be swallowed.
            Threading::Sleep(100);

            const FStringView Text(In.Text);
            int32 Typed = 0;
            for (int32 i = 0; i < Text.Len(); ++Typed)
            {
                State.Characters.Enqueue(DecodeUTF8Codepoint(Text, i));
            }
            return Agent::FToolResult::Ok(Lumina::Format("Typed {} characters.", Typed));
        }

        Agent::FToolResult SendEditorMouse(FEditorInputState& State, const SEditorInputParams& In, SEditorInputResult& Out)
        {
            const bool bPress   = In.Action == "Tap" || In.Action == "Press";
            const bool bRelease = In.Action == "Tap" || In.Action == "Release";
            const bool bMove    = In.Action == "Move" || bPress || bRelease;
            if (!bMove)
            {
                return Agent::FToolResult::Error("Action has to be Tap, Press, Release or Move.");
            }
            if (In.Button < 0 || In.Button > 7)
            {
                return Agent::FToolResult::Error("Button has to be 0 to 7, where 0 is left.");
            }

            if (!FocusEditorGameViewport(Out.bGameInputFocused))
            {
                return Agent::FToolResult::Error("The game thread did not pick up the focus change in time.");
            }

            Out.X = In.X;
            Out.Y = In.Y;
            const int32 HoldMilliseconds = Math::Max(In.HoldMilliseconds > 0 ? In.HoldMilliseconds : 40, 1);

            double WindowX = In.X;
            double WindowY = In.Y;
            (void)Agent::FGameThreadGate::Run([&]()
            {
                if (FInputViewport* Viewport = FInputViewportRegistry::Get().GetActiveViewport())
                {
                    Viewport->GetContext().ContextToWindow(In.X, In.Y, WindowX, WindowY);
                }
            }, Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds());

            FMouseMoveInput Move;
            Move.X = (float)WindowX;
            Move.Y = (float)WindowY;
            State.Moves.Enqueue(Move);

            // The move has to land before the button, or the click resolves against the old position.
            Threading::Sleep(HoldMilliseconds);

            auto Send = [&](bool bPressed)
            {
                FMouseButtonInput Button;
                Button.Button   = static_cast<EMouseKey>(In.Button);
                Button.bPressed = bPressed;
                Button.X        = (float)WindowX;
                Button.Y        = (float)WindowY;
                State.Buttons.Enqueue(Button);
            };

            if (bPress)
            {
                Send(true);
            }

            // Both halves in one pump would leave the button Held before any press edge is evaluated.
            if (bPress && bRelease)
            {
                Threading::Sleep(HoldMilliseconds);
            }

            if (bRelease)
            {
                Send(false);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Sent {} at ({}, {}).", In.Action, In.X, In.Y));
        }

        Agent::FToolResult SendEditorKey(FEditorInputState& State, const SEditorInputParams& In, SEditorInputResult& Out)
        {
            const bool bPress   = In.Action == "Tap" || In.Action == "Press";
            const bool bRelease = In.Action == "Tap" || In.Action == "Release";
            if (!bPress && !bRelease)
            {
                return Agent::FToolResult::Error("Action has to be Tap, Press or Release.");
            }

            if (!FocusEditorGameViewport(Out.bGameInputFocused))
            {
                return Agent::FToolResult::Error("The game thread did not pick up the focus change in time.");
            }

            const int32 HoldMilliseconds = Math::Max(In.HoldMilliseconds > 0 ? In.HoldMilliseconds : 100, 1);

            // ImGui releases the keyboard at the next frame start, so a key sent this frame would be swallowed.
            Threading::Sleep(HoldMilliseconds);

            auto Send = [&](bool bPressed)
            {
                FKeyInput Input;
                Input.Key      = In.Key;
                Input.bPressed = bPressed;
                Input.bCtrl    = In.bCtrl;
                Input.bShift   = In.bShift;
                Input.bAlt     = In.bAlt;
                State.Keys.Enqueue(Input);
            };

            if (bPress)
            {
                Send(true);
            }

            // Both halves in one pump would leave the key Held before any action edge is evaluated.
            if (bPress && bRelease)
            {
                Threading::Sleep(HoldMilliseconds);
            }

            if (bRelease)
            {
                Send(false);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Sent {}.", In.Action));
        }

        Agent::FToolResult ViewEditorGraph(const SEditorGraphParams& In, SEditorGraphResult& Out)
        {
            FString Error;
            SessionOps::FGraphView View;
            if (!SessionOps::GetGraphView(FStringView(In.Tab), View, Error))
            {
                return Agent::FToolResult::Error(Error + " Call editor.tabs List.");
            }

            Out.bDrawn    = View.bDrawn;
            Out.MinX      = View.MinX;
            Out.MinY      = View.MinY;
            Out.MaxX      = View.MaxX;
            Out.MaxY      = View.MaxY;
            Out.Zoom      = View.Zoom;
            Out.NodeCount = View.NodeCount;
            Out.Offscreen = Move(View.Offscreen);

            if (!Out.bDrawn)
            {
                return Agent::FToolResult::Ok("That canvas has not been drawn yet, so it has no view. Focus the tab first.");
            }

            return Agent::FToolResult::Ok(Lumina::Format("Showing ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) at zoom {:.2f}, {} of {} node(s) off screen.",
                Out.MinX, Out.MinY, Out.MaxX, Out.MaxY, Out.Zoom, Out.Offscreen.Num(), Out.NodeCount));
        }

        Agent::FToolResult FrameEditorGraph(const SEditorGraphParams& In, SEditorGraphResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::FrameGraph(FStringView(In.Tab), In.Nodes, Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error);
            }

            return Agent::FToolResult::Ok(In.Nodes.IsEmpty()
                ? Lumina::Format("Framing every node in '{}'.", In.Tab)
                : Lumina::Format("Framing {} node(s) in '{}'.", In.Nodes.Num(), In.Tab));
        }

        Agent::FToolResult ArrangeEditorGraph(const SEditorGraphParams& In, SEditorGraphResult& Out)
        {
            FString Error;
            Out.bDone = SessionOps::ArrangeGraph(FStringView(In.Tab), Error);
            if (!Out.bDone)
            {
                return Agent::FToolResult::Error(Error);
            }

            return Agent::FToolResult::Ok(Lumina::Format("Arranging '{}'.", In.Tab));
        }

        Agent::FToolResult ReloadEditorScripts(const SScriptReloadParams& Params, SScriptReloadResult& Out)
        {
            if (!DotNet::IsInitialized())
            {
                return Agent::FToolResult::Error("The .NET host is not running, so there is nothing to reload.");
            }

            const int32 GateTimeout = Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds();
            int32 Before = 0;
            int32 FinishedBefore = 0;
            if (Agent::FGameThreadGate::Run([&Before, &FinishedBefore]()
                {
                    Before = DotNet::GetScriptGeneration();
                    FinishedBefore = DotNet::GetFinishedScriptReloads();
                    DotNet::RequestScriptReload();
                }, GateTimeout) != Agent::EGameThreadResult::Ran)
            {
                return Agent::FToolResult::Error("The game thread did not pick up the reload request.");
            }

            Out.PreviousGeneration = Before;
            Out.Generation = Before;

            const double Deadline = PlatformTime::Seconds() + Math::Max(0.0f, Params.TimeoutSeconds);
            int32 FailedResult = 0;
            while (PlatformTime::Seconds() < Deadline)
            {
                Threading::Sleep(50);

                int32 Now = Before;
                int32 Finished = FinishedBefore;
                int32 LastResult = 0;
                if (Agent::FGameThreadGate::Run([&Now, &Finished, &LastResult]()
                    {
                        Now = DotNet::GetScriptGeneration();
                        Finished = DotNet::GetFinishedScriptReloads();
                        LastResult = DotNet::GetLastScriptReloadResult();
                    }, GateTimeout) != Agent::EGameThreadResult::Ran)
                {
                    break;
                }
                if (Now != Before)
                {
                    Out.Generation = Now;
                    Out.bReloaded = true;
                    break;
                }
                // A failed compile never changes the generation, so the finished count is what ends the wait.
                if (Finished != FinishedBefore && LastResult != 0)
                {
                    FailedResult = LastResult;
                    break;
                }
            }

            if (FailedResult != 0)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "The reload failed with host error {} and the previous scripts stay loaded; editor.log_tail has the compiler output.",
                    FailedResult));
            }

            DotNet::FScriptDiagnostics Diagnostics{};
            bool bHaveDiagnostics = false;
            (void)Agent::FGameThreadGate::Run([&Diagnostics, &bHaveDiagnostics]()
                {
                    bHaveDiagnostics = DotNet::GetRuntimeDiagnostics(Diagnostics);
                }, GateTimeout);

            if (bHaveDiagnostics)
            {
                Out.AliveContexts = Diagnostics.AliveScriptAlcCount;
                Out.LoadedTypes = Diagnostics.LoadedTypeCount;
            }

            if (!Out.bReloaded)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "The reload is still queued after {} seconds; generation is still {}.",
                    Params.TimeoutSeconds, Out.Generation));
            }

            LOG_INFO("[MCP] An agent reloaded scripts; generation {} -> {}.", Before, Out.Generation);

            if (Out.LoadedTypes == 0)
            {
                return Agent::FToolResult::Ok(Lumina::Format(
                    "Reloaded to generation {}, but no script types registered. Compilation most "
                    "likely failed; read editor.log_tail for the errors.", Out.Generation));
            }

            return Agent::FToolResult::Ok(Lumina::Format(
                "Reloaded to generation {}: {} script type(s), {} script context(s) resident.",
                Out.Generation, Out.LoadedTypes, Out.AliveContexts));
        }

        // Waits off the game thread, since the copy rides along with a frame the editor has yet to present.
        Agent::FToolResult CaptureEditorWindow(const SScreenshotParams& In, SScreenshotResult& Out, int32 GateTimeout)
        {
            uint64 Ticket = 0;
            if (Agent::FGameThreadGate::Run([&Ticket] { Ticket = Render().RequestWindowCapture(); }, GateTimeout)
                != Agent::EGameThreadResult::Ran)
            {
                return Agent::FToolResult::Error("The game thread did not pick up the capture.");
            }

            constexpr double kPresentTimeoutSeconds = 5.0;
            const double Deadline = PlatformTime::Seconds() + kPresentTimeoutSeconds;
            FRenderManager::FWindowCapture Capture;
            bool bTaken = false;
            while (!bTaken && PlatformTime::Seconds() < Deadline)
            {
                Threading::Sleep(16);
                bTaken = Render().TakeWindowCapture(Ticket, Capture);
            }
            if (!bTaken)
            {
                return Agent::FToolResult::Error("The editor presented no frame within 5 seconds, which a minimized window does.");
            }

            const Screenshot::FCaptureResult Saved = Screenshot::SaveWindowCapture(Capture.Pixels, Capture.Width, Capture.Height, In.OutputPath);
            if (!Saved.bSuccess)
            {
                return Agent::FToolResult::Error(Saved.ErrorMessage.IsEmpty() ? FString("Writing the capture failed.") : Saved.ErrorMessage);
            }

            Out.Path   = Saved.OutputPath;
            Out.Width  = static_cast<int32>(Saved.ResolutionX);
            Out.Height = static_cast<int32>(Saved.ResolutionY);
            return Agent::FToolResult::Ok(Lumina::Format("Wrote the {}x{} editor window to {}.", Out.Width, Out.Height, Out.Path));
        }

        Agent::FToolResult TailEditorLog(const SLogTailParams& In, SLogTailResult& Out)
        {
            ELogLevel MinLevel = ELogLevel::Info;
            if (!In.MinLevel.IsEmpty() && !ParseEditorLogLevel(In.MinLevel, MinLevel))
            {
                return Agent::FToolResult::Error("MinLevel has to be Trace, Debug, Info, Warn, Error or Critical.");
            }

            const int32 Count = In.Count > 0 ? In.Count : 50;
            Logging::FLogQueue& Queue = Logging::GetConsoleLogQueue();

            // Walked newest first so the cap keeps the recent lines, then flipped to read in order.
            for (int32 Index = Queue.Num(); Index > 0; --Index)
            {
                const FConsoleMessage& Entry = Queue[Index - 1];
                if (Entry.Level < MinLevel || !ContainsTextFold(FStringView(*Entry.Message), In.Contains))
                {
                    ++Out.Dropped;
                    continue;
                }

                if (Out.Lines.Num() >= Count)
                {
                    ++Out.Dropped;
                    continue;
                }

                SLogLine Line;
                Line.Time    = FString(*Entry.Time);
                Line.Logger  = FString(Entry.LoggerName.GetData(), Entry.LoggerName.Len());
                Line.Level   = EditorLogLevelName(Entry.Level);
                Line.Message = FString(*Entry.Message);
                Out.Lines.Add(Move(Line));
            }

            Algo::Reverse(Out.Lines.begin(), Out.Lines.end());

            FString Text;
            for (const SLogLine& Line : Out.Lines)
            {
                Text.Append(Lumina::Format("[{}] [{}] {}\n", Line.Time, Line.Level, Line.Message));
            }

            return Agent::FToolResult::Ok(Text.IsEmpty() ? FString("No matching log lines.") : Text);
        }

        Agent::FToolResult ExecEditorConsole(const SConsoleExecParams& In, SConsoleExecResult& Out)
        {
            const auto [Line, Name, Value] = FConsoleRegistry::SplitLine(FStringView(In.Line));

            if (Name.IsEmpty())
            {
                return Agent::FToolResult::Error("A console line is needed.");
            }

            // An agent has no business ending the process it is driving.
            for (const char* Denied : { "quit", "exit", "crash" })
            {
                if (Name == Denied)
                {
                    return Agent::FToolResult::Error(Lumina::Format("'{}' is not allowed from an agent.", Name));
                }
            }

            const FConsoleLineResult Result = FConsoleRegistry::Get().ExecuteLine(Line, false);
            switch (Result.Kind)
            {
            case EConsoleLineResult::RanCommand:
                Out.Output = "executed";
                return Agent::FToolResult::Ok(Lumina::Format("Ran {}.", Name));
            case EConsoleLineResult::RejectedValue:
                return Agent::FToolResult::Error(Lumina::Format("'{}' did not accept '{}'.", Name, Value));
            case EConsoleLineResult::ReadVariable:
            case EConsoleLineResult::SetVariable:
                Out.Output = Result.Value;
                return Agent::FToolResult::Ok(Lumina::Format("{} = {}", Name, Out.Output));
            case EConsoleLineResult::Empty:
            case EConsoleLineResult::Unknown:
                break;
            }
            return Agent::FToolResult::Error(Lumina::Format("No console command or variable named '{}'.", Name));
        }

        Agent::FToolResult SetEditorWindow(const SWindowParams& In, SWindowState& Out)
        {
            FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
            if (Window == nullptr)
            {
                return Agent::FToolResult::Error("There is no main window, which a headless editor does not have.");
            }

            if (In.Action == "Resize")
            {
                if (In.Width <= 0 || In.Height <= 0)
                {
                    return Agent::FToolResult::Error("Resize needs a positive Width and Height; use Minimize for a zero-size window.");
                }
                if (Window->IsWindowMaximized() || Window->IsWindowMinimized())
                {
                    Window->Restore();
                }
                Window->SetWindowSize(In.Width, In.Height);
            }
            else if (In.Action == "Move")
            {
                Window->SetWindowPosition(In.X, In.Y);
            }
            else if (In.Action == "Minimize")
            {
                Window->Minimize();
            }
            else if (In.Action == "Restore")
            {
                Window->Restore();
            }
            else if (In.Action == "Maximize")
            {
                Window->Maximize();
            }
            else if (In.Action != "Query")
            {
                return Agent::FToolResult::Error("Action has to be Query, Resize, Move, Minimize, Restore or Maximize.");
            }

            FillEditorWindowState(*Window, Out);
            return Agent::FToolResult::Ok(Lumina::Format("{}x{} at ({}, {}){}{}.", Out.Width, Out.Height, Out.X, Out.Y,
                Out.bMinimized ? ", minimized" : "", Out.bMaximized ? ", maximized" : ""));
        }

        Agent::FToolResult SaveAllEditorPackages(SSaveAllResult& Out)
        {
            uint32 Saved = 0;
            FString Error;
            const bool bAllSaved = SessionOps::SaveAll(Saved, Out.Failed, Error);
            Out.Saved = (int32)Saved;
            if (!bAllSaved)
            {
                return Agent::FToolResult::Error(Error);
            }

            FString WorldError;
            CWorld* SceneWorld = SessionOps::GetSceneWorld(WorldError);
            Out.bWorldUnsaved = SceneWorld != nullptr && SceneWorld->GetPackage() == nullptr;
            if (Out.bWorldUnsaved)
            {
                return Agent::FToolResult::Ok(Lumina::Format("Saved {} package(s), but the open level is a transient world with no package, so its "
                    "entities were not saved. Open a world asset with editor.open and build the level there.", Saved));
            }

            return Agent::FToolResult::Ok(Saved == 0 ? FString("Nothing to save.") : Lumina::Format("Saved {} package(s).", Saved));
        }

        Agent::FToolResult QuitEditor(const SQuitParams& In, SQuitResult& Out)
        {
            if (In.bSaveFirst && In.bDiscardUnsaved)
            {
                return Agent::FToolResult::Error("Pass bSaveFirst or bDiscardUnsaved, not both.");
            }

            const SessionOps::EQuitUnsaved Unsaved = In.bSaveFirst ? SessionOps::EQuitUnsaved::Save
                : In.bDiscardUnsaved ? SessionOps::EQuitUnsaved::Discard
                : SessionOps::EQuitUnsaved::Refuse;

            FString Error;
            Out.bQuitting = SessionOps::Quit(Unsaved, Error);
            if (!Out.bQuitting)
            {
                Out.Unsaved = SessionOps::GetDirtyPackagePaths();
                for (const FString& Path : Out.Unsaved)
                {
                    Error.Append(Lumina::Format(" {}", Path));
                }
                return Agent::FToolResult::Error(Unsaved == SessionOps::EQuitUnsaved::Refuse
                    ? Error + " Call editor.save_all, or pass bSaveFirst or bDiscardUnsaved."
                    : Error);
            }

            LOG_INFO("[MCP] An agent closed the editor.");
            return Agent::FToolResult::Ok("Quitting.");
        }

        void RegisterEditorHistory(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEditorHistoryParams, SEditorHistoryResult>(
                Owner, "editor.history",
                "Undo or Redo steps in the scene target, or in Tab when given, which is any open tab such as a material or animation graph editor.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &StepEditorHistory);
        }

        void RegisterEditorPlay(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEditorPlayParams, SPlayState>(
                Owner, "editor.play",
                "Start, Stop, Pause or Resume play-in-editor on the open world, or report its State. Start can run a networked "
                "session with several players.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEditorPlayParams& In, SPlayState& Out)
                {
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    switch (In.Action)
                    {
                    case EEditorPlayAction::Start:
                        return StartEditorPlay(In, Out);
                    case EEditorPlayAction::State:
                        FillEditorPlayState(Out);
                        return Agent::FToolResult::Ok(Out.bPlaying
                            ? Lumina::Format("Playing{} in {}.", Out.bPaused ? " (paused)" : "", Out.World)
                            : Lumina::Format("Editing {}.", Out.World));
                    default:
                        return ControlEditorPlay(In.Action, Out);
                    }
                });
        }

        void RegisterEditorTabs(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEditorTabsParams, SEditorTabsResult>(
                Owner, "editor.tabs",
                "List open editor tabs, Focus or Close one, Float it to a screen rect, or Target it so the scene, entity and undo "
                "tools act on it, such as a prefab editor. Open shows a Tools menu panel such as Memory. Changes land on the next frames.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEditorTabsParams& In, SEditorTabsResult& Out)
                {
                    switch (In.Action)
                    {
                    case EEditorTabsAction::Focus:  return FocusEditorTab(In, Out);
                    case EEditorTabsAction::Close:  return CloseEditorTab(In, Out);
                    case EEditorTabsAction::Float:  return FloatEditorTab(In, Out);
                    case EEditorTabsAction::Target: return TargetEditorTab(In, Out);
                    case EEditorTabsAction::Open:   return OpenEditorToolPanel(In, Out);
                    default:                        return ListEditorTabs(Out);
                    }
                });
        }

        void RegisterEditorOpen(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEditorOpenParams, SEditorOpenResult>(
                Owner, "editor.open",
                "Open an Asset in its editor tab, a text File in its own editor, or travel the running game to a Level at the start "
                "of the next frame. Set exactly one.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEditorOpenParams& In, SEditorOpenResult& Out)
                {
                    const int32 Set = (In.Asset.IsEmpty() ? 0 : 1) + (In.File.IsEmpty() ? 0 : 1) + (In.Level.IsEmpty() ? 0 : 1);
                    if (Set != 1)
                    {
                        return Agent::FToolResult::Error(Set == 0 ? "Set one of Asset, File or Level." : "Set only one of Asset, File or Level.");
                    }

                    if (!In.Asset.IsEmpty())
                    {
                        return OpenEditorAsset(In.Asset, Out);
                    }
                    if (!In.File.IsEmpty())
                    {
                        return OpenEditorFile(In.File, Out);
                    }
                    return OpenEditorLevel(In.Level, Out);
                });
        }

        // Any thread on purpose: a tap needs the release to land a frame after the press.
        void RegisterEditorInput(FStringView Owner)
        {
            if (FEditorInputState* State = FMCPEditorModule::GetEditorInput())
            {
                State->PumpedHandle = FCoreDelegates::Get().OnInputPumped.AddMember(State, &FEditorInputState::Forward);
            }

            Agent::FToolRegistry::Get().Register<SEditorInputParams, SEditorInputResult>(
                Owner, "editor.input",
                "Send Text, Mouse or Key input to the game viewport as a real device would, giving it input focus first. Mouse "
                "moves to a viewport pixel and optionally clicks.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::Any,
                [](const SEditorInputParams& In, SEditorInputResult& Out)
                {
                    FEditorInputState* State = FMCPEditorModule::GetEditorInput();
                    if (State == nullptr)
                    {
                        return Agent::FToolResult::Error("The MCP editor module is not loaded.");
                    }

                    switch (In.Kind)
                    {
                    case EEditorInputKind::Mouse: return SendEditorMouse(*State, In, Out);
                    case EEditorInputKind::Key:   return SendEditorKey(*State, In, Out);
                    default:                      return SendEditorText(*State, In, Out);
                    }
                });
        }

        void RegisterEditorGraph(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEditorGraphParams, SEditorGraphResult>(
                Owner, "editor.graph",
                "For a material, material function, animation or audio graph tab: View reports the visible canvas rect and offscreen "
                "nodes, Frame pans to Nodes, Arrange lays the canvas out by its links as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEditorGraphParams& In, SEditorGraphResult& Out)
                {
                    switch (In.Action)
                    {
                    case EEditorGraphAction::Frame:   return FrameEditorGraph(In, Out);
                    case EEditorGraphAction::Arrange: return ArrangeEditorGraph(In, Out);
                    default:                          return ViewEditorGraph(In, Out);
                    }
                });
        }

        void RegisterEditorContentBrowser(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SBrowseFolderParams, SBrowseFolderResult>(
                Owner, "editor.content_browser",
                "Show a folder in the content browser, optionally with text in its search box. The tiles build on the next frame.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SBrowseFolderParams& In, SBrowseFolderResult& Out)
                {
                    FString Error;
                    Out.bDone = SessionOps::BrowseContentFolder(FStringView(In.Folder), FStringView(In.Search), Error);
                    if (!Out.bDone)
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("Browsing '{}'.", In.Folder));
                });
        }

        // Any, not GameThread: the reload is serviced at a fixed point in the frame, so polls have to let the game thread run.
        void RegisterEditorReloadScripts(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SScriptReloadParams, SScriptReloadResult>(
                Owner, "editor.reload_scripts",
                "Recompile and reload the project's C# scripts and wait for the new generation. LoadedTypes of zero means "
                "compilation failed; editor.log_tail has the errors.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::Any, &ReloadEditorScripts);
        }

        void RegisterEditorSaveAll(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSaveAllParams, SSaveAllResult>(
                Owner, "editor.save_all",
                "Save every package with unsaved changes, as File > Save All does. Open asset editors do their own save work.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSaveAllParams&, SSaveAllResult& Out) { return SaveAllEditorPackages(Out); });
        }

        void RegisterEditorQuit(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SQuitParams, SQuitResult>(
                Owner, "editor.quit",
                "Close the editor without any prompt. Refuses while packages are unsaved unless bSaveFirst or bDiscardUnsaved. "
                "Play is stopped first and the exit lands a few frames later.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &QuitEditor);
        }

        void RegisterEditorWindow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SWindowParams, SWindowState>(
                Owner, "editor.window",
                "Query, Resize, Move, Minimize, Restore or Maximize the editor's main window. The OS window changes at once; "
                "the swapchain and viewports follow on the next frames.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &SetEditorWindow);
        }

        void RegisterEditorScreenshot(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SScreenshotParams, SScreenshotResult>(
                Owner, "editor.screenshot",
                "Capture the active viewport, or with Source EditorUI the whole editor window, to an image file and report where it landed.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
                [](const SScreenshotParams& In, SScreenshotResult& Out)
                {
                    const int32 GateTimeout = Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds();
                    if (In.Source == "EditorUI")
                    {
                        return CaptureEditorWindow(In, Out, GateTimeout);
                    }

                    Agent::FToolResult Result = Agent::FToolResult::Error("The game thread did not run the capture.");
                    if (Agent::FGameThreadGate::Run([&In, &Out, &Result] { Result = CaptureEditorScene(In, Out); }, GateTimeout)
                        != Agent::EGameThreadResult::Ran)
                    {
                        return Agent::FToolResult::Error("The game thread did not pick up the capture.");
                    }
                    return Result;
                });
        }

        void RegisterEditorLogTail(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SLogTailParams, SLogTailResult>(
                Owner, "editor.log_tail",
                "Return the newest lines from the editor log, filtered by level and text.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread, &TailEditorLog);
        }

        void RegisterEditorConsoleExec(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SConsoleExecParams, SConsoleExecResult>(
                Owner, "editor.console_exec",
                "Run a console command, or read or set a console variable, as the console panel would.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &ExecEditorConsole);
        }
    }

    using namespace MCPEditorSessionToolsInternal;

    void RegisterEditorSessionTools(FStringView Owner)
    {
        RegisterEditorHistory(Owner);
        RegisterEditorPlay(Owner);
        RegisterEditorTabs(Owner);
        RegisterEditorOpen(Owner);
        RegisterEditorInput(Owner);
        RegisterEditorGraph(Owner);
        RegisterEditorContentBrowser(Owner);
        RegisterEditorReloadScripts(Owner);
        RegisterEditorSaveAll(Owner);
        RegisterEditorQuit(Owner);
        RegisterEditorWindow(Owner);
        RegisterEditorScreenshot(Owner);
        RegisterEditorLogTail(Owner);
        RegisterEditorConsoleExec(Owner);
    }

    void UnregisterEditorSessionTools()
    {
        // The pump delegate outlives this DLL, so a stale entry would be destroyed after unload.
        if (FEditorInputState* State = FMCPEditorModule::GetEditorInput())
        {
            FCoreDelegates::Get().OnInputPumped.Remove(State->PumpedHandle);
            State->PumpedHandle = {};
        }
    }

    Agent::FToolResult CaptureEditorScene(const SScreenshotParams& In, SScreenshotResult& Out)
    {
        Screenshot::ECaptureSource Source = Screenshot::ECaptureSource::FinalLDR;
        if (In.Source == "SceneHDR")
        {
            Source = Screenshot::ECaptureSource::SceneHDR;
        }
        else if (!In.Source.IsEmpty() && In.Source != "FinalLDR")
        {
            return Agent::FToolResult::Error("Source has to be FinalLDR, SceneHDR or EditorUI.");
        }

        Screenshot::FCaptureResult Captured;
        if (In.Tab.IsEmpty())
        {
            Captured = Screenshot::CaptureActiveWorld(Source, In.OutputPath);
        }
        else
        {
            FString Error;
            CWorld* World = SessionOps::GetTabWorld(FStringView(In.Tab), Error);
            IRenderScene* Scene = World != nullptr ? World->GetRenderer() : nullptr;
            if (Scene == nullptr)
            {
                return Agent::FToolResult::Error(Error.IsEmpty() ? FString("That tab's world has no renderer right now.") : Error);
            }
            Captured = Screenshot::Capture(Scene, Source, In.OutputPath);
        }
        if (!Captured.bSuccess)
        {
            // A world with no viewport showing it has had its renderer reclaimed, so there is nothing to read back.
            return Agent::FToolResult::Error(Captured.ErrorMessage.IsEmpty()
                ? FString("The capture failed.")
                : Captured.ErrorMessage + " Bring a world viewport tab to the front in the editor and try again.");
        }

        Out.Path   = Captured.OutputPath;
        Out.Width  = static_cast<int32>(Captured.ResolutionX);
        Out.Height = static_cast<int32>(Captured.ResolutionY);

        return Agent::FToolResult::Ok(Lumina::Format("Wrote {}x{} to {}.", Out.Width, Out.Height, Out.Path));
    }

    void FEditorInputState::Forward()
    {
        FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        if (Window == nullptr)
        {
            return;
        }

        FKeyInput Input;
        while (Keys.TryDequeue(Input))
        {
            Window->OnKey.Broadcast(Window, Input);
        }

        uint32 Codepoint;
        while (Characters.TryDequeue(Codepoint))
        {
            Window->OnChar.Broadcast(Window, Codepoint);
        }

        // Moves before buttons, so a click is evaluated at the position it was aimed at.
        FMouseMoveInput Move;
        while (Moves.TryDequeue(Move))
        {
            Window->OnMouseMove.Broadcast(Window, Move);
        }

        FMouseButtonInput Button;
        while (Buttons.TryDequeue(Button))
        {
            Window->OnMouseButton.Broadcast(Window, Button);
        }
    }
}
