#include "Core/Application/Application.h"
#include "Memory/MemoryTracking.h"
#include "Core/Application/ApplicationGlobalState.h"
#if WITH_EDITOR
#include "LuminaEditor.h"
#endif
#include <exception>
#include "Config/Config.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Memory/Memory.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Log/Log.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"

using namespace Lumina;




int LuminaMain(int ArgC, char** ArgV)  // NOLINT(misc-use-internal-linkage)
{
    // Reversed, the uploader would swallow the crash before anything was written to disk.
    CrashReporting::Initialize();
    CrashHandler::Install();

    HangWatchdog::Start();

    int Result = 0;
    FApplicationGlobalState GlobalState;

    // A crash before a project opens would otherwise upload with no log attached at all.
    CrashReporting::AddAttachment(Logging::GetLogFilePath());

    // Every log now states plainly whether crashes will be uploaded, instead of leaving it inferred.
    CrashReporting::LogStatus();

    FCommandLine Parsed{ArgC, ArgV};
    GCommandLine = &Parsed;

#if LUMINA_MEMORY_TRACKING
    // Steady-state memory predates the profiler window, so naming it needs capture from allocation one.
    Memory::SetCaptureCallstacks(Parsed.Has("memcallstacks"));
#endif

    FApplication Application{};
    GApp = &Application;

    FConfig Config{};
    GConfig = &Config;

    #if WITH_EDITOR
    FEditorEngine EdEngine{};
    GEditorEngine = &EdEngine;
    GEngine = GEditorEngine;
    #else
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
    CrashReporting::Shutdown();
    return Result;
}
