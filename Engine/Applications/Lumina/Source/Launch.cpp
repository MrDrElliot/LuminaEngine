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
#include "Log/Log.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"

using namespace Lumina;


DECLARE_MODULE_ALLOCATOR_OVERRIDES();


int LuminaMain(int ArgC, char** ArgV)  // NOLINT(misc-use-internal-linkage)
{
    // Order matters. The reporter installs its unhandled-exception filter first so CrashHandler's
    // install captures it as the previous filter; that is what lets the local dump be written and
    // then handed off to the uploader. Reversed, the uploader would swallow the crash before
    // anything was written to disk.
    CrashReporting::Initialize();
    CrashHandler::Install();

    HangWatchdog::Start();

    int Result = 0;
    FApplicationGlobalState GlobalState;

    // Attach the engine-side log now that logging exists. LoadProject re-points this at the
    // project's copy later; without it here, a crash before a project opens -- during renderer
    // init, or sitting on the Open Project dialog -- would upload with no log at all.
    CrashReporting::AddAttachment(Logging::GetLogFilePath());

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
