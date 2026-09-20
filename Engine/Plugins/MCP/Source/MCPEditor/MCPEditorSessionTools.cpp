#include "MCPEditorSessionTools.h"

#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolRegistry.h"
#include "Containers/ConcurrentQueue.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowInput.h"
#include "Core/Threading/Thread.h"
#include "Input/InputViewport.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Log/Log.h"
#include "Session/SessionOps.h"
#include "MCPTextMatch.h"
#include "Tools/Screenshot/ScreenshotCapture.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace
    {
        constexpr const char* GNoWorldEditorSession = "No world editor is open.";

        void FillPlayState(SPlayState& Out)
        {
            FString Error;
            SessionOps::FPlayState State;
            SessionOps::GetPlayState(State, Error);

            Out.bPlaying = State.bPlaying;
            Out.bPaused  = State.bPaused;
            Out.World    = State.World;
        }

        const char* LevelName(ELogLevel Level)
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

        bool ParseLevel(const FString& Text, ELogLevel& OutLevel)
        {
            for (uint8 Index = 0; Index <= static_cast<uint8>(ELogLevel::Critical); ++Index)
            {
                const ELogLevel Level = static_cast<ELogLevel>(Index);
                if (EqualsTextFold(FStringView(LevelName(Level)), FStringView(Text)))
                {
                    OutLevel = Level;
                    return true;
                }
            }

            return false;
        }

        void RegisterUndoRedo(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SUndoParams, SUndoResult>(
                Owner, "editor.undo",
                "Undo the world editor's last steps, including any scene change an agent tool made.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SUndoParams& In, SUndoResult& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Undo is blocked while playing in editor.");
                    }

                    for (int32 Step = 0; Step < Math::Max(In.Steps, 1) && SessionOps::Undo(1) > 0; ++Step)
                    {
                        ++Out.Applied;
                    }

                    const SessionOps::FUndoState Undo = SessionOps::GetUndoState();
                    Out.NextUndo = Undo.NextUndo;
                    Out.NextRedo = Undo.NextRedo;

                    if (Out.Applied == 0)
                    {
                        return Agent::FToolResult::Error("Nothing to undo.");
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Undid {} step(s). Next undo: {}.",
                        Out.Applied, Out.NextUndo.empty() ? FString("nothing") : Out.NextUndo));
                });

            Agent::FToolRegistry::Get().Register<SUndoParams, SUndoResult>(
                Owner, "editor.redo",
                "Redo steps the world editor last undid.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SUndoParams& In, SUndoResult& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Redo is blocked while playing in editor.");
                    }

                    for (int32 Step = 0; Step < Math::Max(In.Steps, 1) && SessionOps::Redo(1) > 0; ++Step)
                    {
                        ++Out.Applied;
                    }

                    const SessionOps::FUndoState Undo = SessionOps::GetUndoState();
                    Out.NextUndo = Undo.NextUndo;
                    Out.NextRedo = Undo.NextRedo;

                    if (Out.Applied == 0)
                    {
                        return Agent::FToolResult::Error("Nothing to redo.");
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Redid {} step(s). Next redo: {}.",
                        Out.Applied, Out.NextRedo.empty() ? FString("nothing") : Out.NextRedo));
                });
        }

        void RegisterPlayControl(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPlayStateParams, SPlayState>(
                Owner, "editor.play_state",
                "Report whether the editor is playing, paused, and which world is open.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPlayStateParams&, SPlayState& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    FillPlayState(Out);
                    return Agent::FToolResult::Ok(Out.bPlaying
                        ? Lumina::Format("Playing{} in {}.", Out.bPaused ? " (paused)" : "", Out.World)
                        : Lumina::Format("Editing {}.", Out.World));
                });

            Agent::FToolRegistry::Get().Register<SPlayStateParams, SPlayState>(
                Owner, "editor.play",
                "Start play-in-editor on the open world.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPlayStateParams&, SPlayState& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    if (!SessionOps::StartPlay(SessionError))
                    {
                        FillPlayState(Out);
                        return Agent::FToolResult::Error("Already playing.");
                    }

                    LOG_INFO("[MCP] An agent started play-in-editor.");
                    FillPlayState(Out);
                    return Agent::FToolResult::Ok("Playing.");
                });

            Agent::FToolRegistry::Get().Register<SPlayStateParams, SPlayState>(
                Owner, "editor.stop",
                "Stop play-in-editor or simulation and return to editing.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPlayStateParams&, SPlayState& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    if (!SessionOps::IsSimulating())
                    {
                        FillPlayState(Out);
                        return Agent::FToolResult::Error("Nothing is playing.");
                    }

                    SessionOps::StopPlay(SessionError);
                    LOG_INFO("[MCP] An agent stopped play-in-editor.");
                    FillPlayState(Out);
                    return Agent::FToolResult::Ok("Stopped.");
                });

            Agent::FToolRegistry::Get().Register<SPauseParams, SPlayState>(
                Owner, "editor.pause",
                "Pause or resume the running play-in-editor session.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPauseParams& In, SPlayState& Out)
                {
                    FString SessionError;
                    if (!SessionOps::HasSceneEditor())
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorSession);
                    }

                    if (!SessionOps::IsSimulating())
                    {
                        FillPlayState(Out);
                        return Agent::FToolResult::Error("Nothing is playing.");
                    }

                    SessionOps::SetPaused(In.bPaused, SessionError);
                    FillPlayState(Out);
                    return Agent::FToolResult::Ok(In.bPaused ? "Paused." : "Resumed.");
                });
        }

        void RegisterTabs(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListTabsParams, SListTabsResult>(
                Owner, "editor.list_tabs",
                "List every open editor tab, which asset it edits, and which one has focus.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListTabsParams&, SListTabsResult& Out)
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
                        Out.Tabs.push_back(Move(Info));
                    });

                    FString Text;
                    for (const STabInfo& Tab : Out.Tabs)
                    {
                        Text.append(Lumina::Format("{}{}{}\n", Tab.Name, Tab.bFocused ? " (focused)" : "", Tab.bUnsaved ? " *" : ""));
                    }

                    return Agent::FToolResult::Ok(Text.empty() ? FString("No tabs are open.") : Text);
                });

            Agent::FToolRegistry::Get().Register<SOpenAssetParams, SOpenAssetResult>(
                Owner, "editor.open_asset",
                "Open an asset in its editor tab, or focus the tab it already has. Edits made while open are undoable.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SOpenAssetParams& In, SOpenAssetResult& Out)
                {
                    const TOptional<FGuid> Guid = FGuid::TryParse(FStringView(In.Asset));
                    if (!Guid.IsSet())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a GUID.", In.Asset));
                    }

                    FString Error;
                    if (!SessionOps::OpenAsset(*Guid, Out.Tab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Open in tab '{}'.", Out.Tab));
                });

            Agent::FToolRegistry::Get().Register<STabNameParams, STabActionResult>(
                Owner, "editor.focus_tab",
                "Bring a tab to the front. Focus lands on the next frame.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const STabNameParams& In, STabActionResult& Out)
                {
                    FString Error;
                    Out.bDone = SessionOps::FocusTab(FStringView(In.Tab), Error);
                    if (!Out.bDone)
                    {
                        return Agent::FToolResult::Error(Error + " Call editor.list_tabs.");
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Focusing '{}'.", In.Tab));
                });

            Agent::FToolRegistry::Get().Register<SCloseTabParams, STabActionResult>(
                Owner, "editor.close_tab",
                "Close a tab as its X button would. Refuses unsaved changes unless bDiscardUnsaved.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCloseTabParams& In, STabActionResult& Out)
                {
                    FString Error;
                    Out.bDone = SessionOps::CloseTab(FStringView(In.Tab), In.bDiscardUnsaved, Error);
                    if (!Out.bDone)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Closing '{}'.", In.Tab));
                });
        }

        void RegisterScreenshot(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SScreenshotParams, SScreenshotResult>(
                Owner, "editor.screenshot",
                "Capture the active viewport to an image file and report where it landed.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SScreenshotParams& In, SScreenshotResult& Out)
                {
                    Screenshot::ECaptureSource Source = Screenshot::ECaptureSource::FinalLDR;
                    if (In.Source == "SceneHDR")
                    {
                        Source = Screenshot::ECaptureSource::SceneHDR;
                    }
                    else if (!In.Source.empty() && In.Source != "FinalLDR")
                    {
                        return Agent::FToolResult::Error("Source has to be FinalLDR or SceneHDR.");
                    }

                    const Screenshot::FCaptureResult Captured = Screenshot::CaptureActiveWorld(Source, In.OutputPath);
                    if (!Captured.bSuccess)
                    {
                        // A world with no viewport showing it has had its renderer reclaimed, so there is nothing to read back.
                        return Agent::FToolResult::Error(Captured.ErrorMessage.empty()
                            ? FString("The capture failed.")
                            : Captured.ErrorMessage + " Bring a world viewport tab to the front in the editor and try again.");
                    }

                    Out.Path   = Captured.OutputPath;
                    Out.Width  = static_cast<int32>(Captured.ResolutionX);
                    Out.Height = static_cast<int32>(Captured.ResolutionY);

                    return Agent::FToolResult::Ok(Lumina::Format("Wrote {}x{} to {}.", Out.Width, Out.Height, Out.Path));
                });
        }

        void RegisterLogTail(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SLogTailParams, SLogTailResult>(
                Owner, "editor.log_tail",
                "Return the newest lines from the editor log, so a failure can be read rather than guessed.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SLogTailParams& In, SLogTailResult& Out)
                {
                    ELogLevel MinLevel = ELogLevel::Info;
                    if (!In.MinLevel.empty() && !ParseLevel(In.MinLevel, MinLevel))
                    {
                        return Agent::FToolResult::Error("MinLevel has to be Trace, Debug, Info, Warn, Error or Critical.");
                    }

                    const int32 Count = In.Count > 0 ? In.Count : 50;
                    Logging::FLogQueue& Queue = Logging::GetConsoleLogQueue();

                    // Walked newest first so the cap keeps the recent lines, then flipped to read in order.
                    for (size_t Index = Queue.size(); Index > 0; --Index)
                    {
                        const FConsoleMessage& Entry = Queue[Index - 1];
                        if (Entry.Level < MinLevel || !ContainsTextFold(FStringView(Entry.Message.c_str()), In.Contains))
                        {
                            ++Out.Dropped;
                            continue;
                        }

                        if (static_cast<int32>(Out.Lines.size()) >= Count)
                        {
                            ++Out.Dropped;
                            continue;
                        }

                        SLogLine Line;
                        Line.Time    = FString(Entry.Time.c_str());
                        Line.Logger  = FString(Entry.LoggerName.data(), Entry.LoggerName.size());
                        Line.Level   = LevelName(Entry.Level);
                        Line.Message = FString(Entry.Message.c_str());
                        Out.Lines.push_back(Move(Line));
                    }

                    Algo::Reverse(Out.Lines.begin(), Out.Lines.end());

                    FString Text;
                    for (const SLogLine& Line : Out.Lines)
                    {
                        Text.append(Lumina::Format("[{}] [{}] {}\n", Line.Time, Line.Level, Line.Message));
                    }

                    return Agent::FToolResult::Ok(Text.empty() ? FString("No matching log lines.") : Text);
                });
        }

        void RegisterConsoleExec(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SConsoleExecParams, SConsoleExecResult>(
                Owner, "editor.console_exec",
                "Run a console command, or read or set a console variable, as the console panel would.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SConsoleExecParams& In, SConsoleExecResult& Out)
                {
                    const FStringView Line(In.Line);
                    const size_t Space = Line.find_first_of(' ');
                    const FStringView Name  = Space == FStringView::npos ? Line : Line.substr(0, Space);
                    const FStringView Value = Space == FStringView::npos ? FStringView() : Line.substr(Space + 1);

                    if (Name.empty())
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

                    FConsoleRegistry& Registry = FConsoleRegistry::Get();

                    if (Registry.FindCommand(Name) != nullptr)
                    {
                        Registry.ExecuteCommand(Name);
                        Out.Output = "executed";
                        return Agent::FToolResult::Ok(Lumina::Format("Ran {}.", Name));
                    }

                    if (Registry.Find(Name) == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No console command or variable named '{}'.", Name));
                    }

                    if (!Value.empty() && !Registry.SetValueFromString(Name, Value))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' did not accept '{}'.", Name, Value));
                    }

                    const TOptional<FString> Current = Registry.GetValueAsString(Name);
                    Out.Output = Current.IsSet() ? *Current : FString("<unprintable>");

                    return Agent::FToolResult::Ok(Lumina::Format("{} = {}", Name, Out.Output));
                });
        }

        // Drained at the event pump so a key takes a real key's path and frame; anything later is already Held.
        TConcurrentQueue<FKeyInput> InjectedKeys;
        FDelegateHandle InputPumpedHandle;

        void ForwardInjectedKeys()
        {
            FWindow* Window = Windowing::GetPrimaryWindowHandle();
            FKeyInput Input;
            while (InjectedKeys.TryDequeue(Input))
            {
                Window->OnKey.Broadcast(Window, Input);
            }
        }

        // Runs on the transport thread on purpose: a tap needs the release to land a frame after the press.
        void RegisterSendKey(FStringView Owner)
        {
            InputPumpedHandle = FCoreDelegates::OnInputPumped.AddStatic(&ForwardInjectedKeys);

            Agent::FToolRegistry::Get().Register<SSendKeyParams, SSendKeyResult>(
                Owner, "editor.send_key",
                "Send a key to the editor as if typed, giving the game viewport input focus first. "
                "Use it to drive play-in-editor: open menus, toggle panels, trigger bindings.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::Any,
                [](const SSendKeyParams& In, SSendKeyResult& Out)
                {
                    const bool bPress   = In.Action == "Tap" || In.Action == "Press";
                    const bool bRelease = In.Action == "Tap" || In.Action == "Release";
                    if (!bPress && !bRelease)
                    {
                        return Agent::FToolResult::Error("Action has to be Tap, Press or Release.");
                    }

                    auto OnGameThread = [](TMoveOnlyFunction<void()>&& Work)
                    {
                        return Agent::FGameThreadGate::Run(Move(Work), Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds())
                            == Agent::EGameThreadResult::Ran;
                    };

                    const bool bFocused = OnGameThread([&]()
                    {
                        FInputViewportRegistry& Viewports = FInputViewportRegistry::Get();
                        Viewports.SetGameInputFocused(true);

                        // Keys route to the focused viewport, which the editor only sets while its OS window is foreground.
                        FString SceneError;
                        if (CWorld* World = SessionOps::GetSceneWorld(SceneError))
                        {
                            if (FInputViewport* Viewport = Viewports.FindViewportForWorld(World))
                            {
                                Viewports.SetActiveViewport(Viewport);
                                Viewports.SetFocusedViewport(Viewport);
                            }
                        }
                        Out.bGameInputFocused = Viewports.GetFocusedViewport() != nullptr;
                    });
                    if (!bFocused)
                    {
                        return Agent::FToolResult::Error("The game thread did not pick up the focus change in time.");
                    }

                    // ImGui releases the keyboard at the next frame start, so a key sent this frame would be swallowed.
                    Threading::Sleep(Math::Max(In.HoldMilliseconds, 1));

                    auto Send = [&](bool bPressed)
                    {
                        FKeyInput Input;
                        Input.Key      = In.Key;
                        Input.bPressed = bPressed;
                        Input.bCtrl    = In.bCtrl;
                        Input.bShift   = In.bShift;
                        Input.bAlt     = In.bAlt;
                        InjectedKeys.Enqueue(Input);
                    };

                    if (bPress)
                    {
                        Send(true);
                    }

                    // Both halves in one pump would leave the key Held before any action edge is evaluated.
                    if (bPress && bRelease)
                    {
                        Threading::Sleep(Math::Max(In.HoldMilliseconds, 1));
                    }

                    if (bRelease)
                    {
                        Send(false);
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Sent {}.", In.Action));
                });
        }
    }

    void RegisterEditorSessionTools(FStringView Owner)
    {
        RegisterSendKey(Owner);
        RegisterUndoRedo(Owner);
        RegisterPlayControl(Owner);
        RegisterTabs(Owner);
        RegisterScreenshot(Owner);
        RegisterLogTail(Owner);
        RegisterConsoleExec(Owner);
    }

    void UnregisterEditorSessionTools()
    {
        // The pump delegate outlives this DLL, so a stale entry would be destroyed after unload.
        FCoreDelegates::OnInputPumped.Remove(InputPumpedHandle);
        InputPumpedHandle = {};
    }
}
