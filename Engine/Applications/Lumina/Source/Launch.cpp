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
#include "Core/Diagnostics/BenchmarkRun.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Log/Log.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"

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

    // A package keeps third-party DLLs in its data folder, and slang is delay-loaded so its first load comes after this.
    if (const FString PluginsDir = Paths::GetGameDataDirectory() + "/Plugins"; Filesystem::Exists(PluginsDir))
    {
        Platform::PushDLLDirectory(UTF8_TO_TCHAR(PluginsDir.c_str()));
    }

    Benchmark::ParseCommandLine(Parsed);

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
    // Bots are client worlds nobody watches, so a bot process draws and plays nothing either.
    GIsHeadless = USING(WITH_SERVER) || Parsed.Has("server") || Parsed.Has("bots") || Parsed.Has("netbots");
    CrashHandler::SetAllowModalDialog(!GIsHeadless);

    FEngine Engine{};
    GEngine = &Engine;

    // Named so a standalone run stops rotating the log the editor that launched it is still writing.
    if (TOptional<FFixedString> LogName = Parsed.Get("logfile"))
    {
        Logging::SetLogFileName(LogName.value());
    }

    // A project on the command line is a standalone run straight out of the project tree, which
    // FEngine::LoadProject mounts and starts itself. Only a packaged game has paks to mount.
    if (!Parsed.Has("project"))
    {
        (void)FCoreDelegates::OnPreEngineInit.AddLambda([]
        {
            GEngine->MountCookedRuntime();
        });
        (void)FCoreDelegates::OnPostEngineInit.AddLambda([]
        {
            GEngine->StartCookedGame();
        });
    }
    else
    {
        LOG_DISPLAY("Standalone: running the project tree uncooked, so no pak is mounted.");
    }
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
