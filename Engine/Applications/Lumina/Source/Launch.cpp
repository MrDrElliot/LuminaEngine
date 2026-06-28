#include "Core/Application/Application.h"
#include "Core/Application/ApplicationGlobalState.h"
#if WITH_EDITOR
#include "LuminaEditor.h"
#endif
#include <exception>
#include <print>
#include "Config/Config.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Memory/Memory.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Platform/CrashHandler.h"

using namespace Lumina;

// Lumina.exe links Runtime.dll but is its own module: without this, plain new/delete
// in the exe's own TUs (and any exe-only third-party C++) hit the CRT allocator and
// escape the tracker. Routes them to Memory::Malloc like every other module.
DECLARE_MODULE_ALLOCATOR_OVERRIDES();


int LuminaMain(int ArgC, char** ArgV)  // NOLINT(misc-use-internal-linkage)
{
    // Must come before anything else so an early-init fault still produces a
    // dump and a modal.
    CrashHandler::Install();

    // Watches the main-thread heartbeat (bumped each frame in FEngine::Update) and dumps every
    // thread's call stack on a stall. No-op in Shipping / non-Windows.
    HangWatchdog::Start();

    int Result = 0;
    FApplicationGlobalState GlobalState;

    FCommandLine Parsed{ArgC, ArgV};
    GCommandLine = &Parsed;

    FApplication Application{};
    GApp = &Application;

    FConfig Config{};
    GConfig = &Config;

    #if WITH_EDITOR
    FEditorEngine EdEngine{};
    GEditorEngine = &EdEngine;
    GEngine = GEditorEngine;
    #else
    // -server boots a packaged dedicated server: no window, no RHI, no audio, no UI. Must be set
    // before the window is created and before GEngine->Init().
    GIsHeadless = Parsed.Has("server");

    FEngine Engine{};
    GEngine = &Engine;

    FCoreDelegates::OnPreEngineInit.AddLambda([]
    {
        GEngine->MountCookedRuntime();
    });
    FCoreDelegates::OnPostEngineInit.AddLambda([]
    {
        GEngine->StartCookedGame();
    });
    #endif

    Result = Application.Run(ArgC, ArgV);

    #if WITH_EDITOR
    GEditorEngine   = nullptr;
    #endif
    GApp            = nullptr;
    GCommandLine    = nullptr;
    GConfig         = nullptr;

    HangWatchdog::Stop();
    CrashHandler::Shutdown();
    return Result;
}
