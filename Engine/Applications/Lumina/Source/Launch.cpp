#include "Core/Application/Application.h"
#include "Memory/MemoryTracking.h"
#include "Core/Application/ApplicationGlobalState.h"
#if WITH_EDITOR
#include "LuminaEditor.h"
#endif
#include "Config/Config.h"
#include "Core/CommandLine/CommandLine.h"
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
    CrashHandler::SetAllowModalDialog(false);
    CrashHandler::Install();

    HangWatchdog::Start();

    int Result = 0;
    FApplicationGlobalState GlobalState;

    // A crash before a project opens would otherwise upload with no log attached at all.
    CrashReporting::AddAttachment(Logging::GetLogFilePath());

    // Every log now states plainly whether crashes will be uploaded, instead of leaving it inferred.
    CrashReporting::LogStatus();

    FCommandLine Parsed{ArgC, ArgV};
    Global::GCommandLine = &Parsed;

    // A package keeps third-party DLLs in its data folder, and slang is delay-loaded so its first load comes after this.
    if (const FString PluginsDir = Paths::GetGameDataDirectory() + "/Plugins"; Filesystem::Exists(PluginsDir))
    {
        Platform::PushDLLDirectory(UTF8_TO_TCHAR(*PluginsDir));
    }

    Benchmark::ParseCommandLine(Parsed);

#if LUMINA_MEMORY_TRACKING
    // Steady-state memory predates the profiler window, so naming it needs capture from allocation one.
    Memory::SetCaptureCallstacks(Parsed.Has("memcallstacks"));
#endif

    FApplication Application{};
    Global::GApp = &Application;

    FConfig Config{};
    Global::GConfig = &Config;

    #if WITH_EDITOR
    FEditorEngine EdEngine{};
    Global::GEditorEngine = &EdEngine;
    Global::GEngine = Global::GEditorEngine;
    #else
    // Bots are client worlds nobody watches, so a bot process draws and plays nothing either.
    Global::GIsHeadless = USING(WITH_SERVER) || Parsed.Has("server") || Parsed.Has("bots") || Parsed.Has("netbots");

    FEngine Engine{};
    Global::GEngine = &Engine;
    if (!Parsed.Has("project"))
    {
        Engine.EnableCookedRuntimeStartup();
    }

    // Named so a standalone run stops rotating the log the editor that launched it is still writing.
    if (TOptional<FFixedString> LogName = Parsed.Get("logfile"))
    {
        Logging::SetLogFileName(LogName.GetValue());
        CrashReporting::ClearAttachments();
        CrashReporting::AddAttachment(Logging::GetLogFilePath());
    }
    if (Parsed.Has("project"))
    {
        LOG_DISPLAY("Standalone: running the project tree uncooked, so no pak is mounted.");
    }
    #endif

    CrashHandler::SetAllowModalDialog(!Global::GIsHeadless);

    // A second editor on a project already open hands focus to the first and leaves before opening a window.
    #if WITH_EDITOR
    if (EdEngine.ClaimStartupProject())
    #endif
    {
        Result = Application.Run(ArgC, ArgV);
    }

    #if WITH_EDITOR
    Global::GEditorEngine   = nullptr;
    #endif
    Global::GEngine         = nullptr;
    Global::GApp            = nullptr;
    Global::GCommandLine    = nullptr;
    Global::GConfig         = nullptr;

    HangWatchdog::Stop();
    CrashHandler::Shutdown();
    CrashReporting::Shutdown();
    return Result;
}
