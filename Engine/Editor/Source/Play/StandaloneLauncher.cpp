#include "StandaloneLauncher.h"

#include "Core/Engine/Engine.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Containers/StringFormat.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/ImGuiX.h"

#include <atomic>

namespace Lumina
{
    namespace
    {
        struct FLaunchSession
        {
            std::atomic<bool>           bDone{false};
            std::atomic<bool>           bSuccess{false};
            FString                     ErrorMessage;
            FStandaloneLaunchOptions    Options;
        };

        TSharedPtr<FLaunchSession>  GSession;
        FThread                     GWorker;

        FString StandaloneLauncherJoin(FStringView Left, FStringView Right)
        {
            FString Result(Left.GetData(), Left.Len());
            if (!Result.IsEmpty() && Result.Last() != '/' && Result.Last() != '\\')
            {
                Result.AppendChar('/');
            }
            Result.Append(Right.GetData(), Right.Len());
            return Result;
        }

        FString EngineBinariesDirectory()
        {
            return StandaloneLauncherJoin(Paths::GetEngineInstallDirectory(), StandaloneLauncherJoin("Binaries", LUMINA_PLATFORM_NAME));
        }

        FString GameExecutablePath()
        {
            const FFixedString Name = Paths::MakeGameApplicationName();
            return StandaloneLauncherJoin(EngineBinariesDirectory(), FStringView(*Name, Name.Len()));
        }

        FString ProjectDirectory()
        {
            const FStringView Path = GEngine != nullptr ? GEngine->GetProjectPath() : FStringView();
            return FString(Path.GetData(), Path.Len());
        }

        FString ProjectName()
        {
            const FStringView Name = GEngine != nullptr ? GEngine->GetProjectName() : FStringView();
            return FString(Name.GetData(), Name.Len());
        }

        FString ProjectBinaryPath(const FFixedString& FileName)
        {
            const FString Directory = ProjectDirectory();
            if (Directory.IsEmpty())
            {
                return {};
            }
            return StandaloneLauncherJoin(StandaloneLauncherJoin(Directory, StandaloneLauncherJoin("Binaries", LUMINA_PLATFORM_NAME)),
                        FStringView(*FileName, FileName.Len()));
        }

        // A project with no editor-flavor module has no C++ at all, so a game build would produce nothing.
        bool ProjectHasNativeModule()
        {
            const FString EditorModule = ProjectBinaryPath(Paths::MakeModuleFileName(ProjectName()));
            return !EditorModule.IsEmpty() && Filesystem::Exists(EditorModule);
        }

        FString ProjectGameModulePath()
        {
            return ProjectBinaryPath(Paths::MakeGameModuleFileName(ProjectName()));
        }

        FString BuildArguments()
        {
            FString Args = "Build ";
            Args += ProjectName();
            Args += " -TargetType=Game -Configuration=";
            Args += LUMINA_CONFIGURATION_NAME;
            Args += " -Project=\"";
            Args += ProjectDirectory();
            Args += "\"";
            return Args;
        }

        bool StartGameProcess(const FStandaloneLaunchOptions& Options, FString& OutError)
        {
            const FString Executable = GameExecutablePath();
            if (!Filesystem::Exists(Executable))
            {
                OutError = Format("The game application is missing at {}. Build the Game target first.",
                                  *Executable);
                return false;
            }

            const FStringView ProjectFile = GEngine->GetProjectFile();
            if (ProjectFile.IsEmpty())
            {
                OutError = "No .lproject file for the loaded project, so the game has nothing to open.";
                return false;
            }

            FString Params = "--Project=\"";
            Params += ProjectFile;
            Params += "\"";

            if (!Options.MapPath.IsEmpty())
            {
                Params += " --map=";
                Params += Options.MapPath;
            }

            // Its own log, or its startup rotates the file this editor is still writing.
            Params += " --logfile=Standalone";

            FString Quoted = "\"";
            Quoted += Executable;
            Quoted += "\"";

            const int Error = Platform::LaunchProcess(UTF8_TO_TCHAR(*Quoted), UTF8_TO_TCHAR(*Params));
            if (Error != 0)
            {
                OutError = Format("Could not start {} (system error {}).", *Executable, Error);
                return false;
            }

            LOG_DISPLAY("Standalone: launched {} {}", *Executable, *Params);
            return true;
        }

        void LaunchOrReport(const FStandaloneLaunchOptions& Options)
        {
            FString Error;
            if (StartGameProcess(Options, Error))
            {
                ImGuiX::Notifications::NotifySuccess("Standalone game launched.");
                return;
            }

            LOG_ERROR("Standalone: {}", *Error);
            ImGuiX::Notifications::NotifyError("{}", *Error);
        }
    }

    bool FStandaloneLauncher::HasGameBinaries()
    {
        if (!Filesystem::Exists(GameExecutablePath()))
        {
            return false;
        }

        if (ProjectHasNativeModule() && !Filesystem::Exists(ProjectGameModulePath()))
        {
            return false;
        }

        return true;
    }

    FString FStandaloneLauncher::GetBuildCommandLine()
    {
        return Paths::GetBuildToolScript() + " " + BuildArguments();
    }

    bool FStandaloneLauncher::IsBuilding()
    {
        return GSession != nullptr;
    }

    void FStandaloneLauncher::Request(const FStandaloneLaunchOptions& Options)
    {
        if (GEngine == nullptr || !GEngine->HasLoadedProject())
        {
            ImGuiX::Notifications::NotifyError("Open a project before launching standalone.");
            return;
        }

        if (IsBuilding())
        {
            return;
        }

        // An existence check cannot tell a stale project module from a current one, and LBT is incremental.
        if (!Options.bBuildIfMissing)
        {
            if (HasGameBinaries())
            {
                LaunchOrReport(Options);
                return;
            }

            const FString Command = GetBuildCommandLine();
            LOG_ERROR("Standalone: the project's Game binaries are missing. Run: {}", *Command);
            ImGuiX::Notifications::NotifyError("Game binaries are missing. Run: {}", *Command);
            return;
        }

        ImGuiX::Notifications::NotifyInfo("Building the Game target for standalone play.");

        GSession = MakeShared<FLaunchSession>();
        GSession->Options = Options;

        TSharedPtr<FLaunchSession> Session = GSession;
        const FString Tool = Paths::GetBuildToolScript();
        const FString Args = BuildArguments();
        const FString WorkingDirectory(Paths::GetEngineInstallDirectory());

        GWorker = FThread([Session, Tool, Args, WorkingDirectory]()
        {
            const int ExitCode = Platform::RunProcessAndWaitCapture(
                UTF8_TO_TCHAR(*Tool),
                UTF8_TO_TCHAR(*Args),
                UTF8_TO_TCHAR(*WorkingDirectory),
                [](FStringView Line)
                {
                    if (!Line.IsEmpty())
                    {
                        LOG_INFO("[Standalone build] {}", *FString(Line.GetData(), Line.Len()));
                    }
                });

            if (ExitCode != 0)
            {
                Session->ErrorMessage = Format("Game build failed (exit code {}). See the log above.", ExitCode);
            }

            Session->bSuccess.store(ExitCode == 0, std::memory_order_release);
            Session->bDone.store(true, std::memory_order_release);
        });
    }

    void FStandaloneLauncher::Tick()
    {
        if (GSession == nullptr || !GSession->bDone.load(std::memory_order_acquire))
        {
            return;
        }

        if (GWorker.joinable())
        {
            GWorker.join();
        }

        TSharedPtr<FLaunchSession> Finished = Move(GSession);
        GSession = nullptr;

        if (!Finished->bSuccess.load(std::memory_order_acquire))
        {
            LOG_ERROR("Standalone: {}", *Finished->ErrorMessage);
            ImGuiX::Notifications::NotifyError("{}", *Finished->ErrorMessage);
            return;
        }

        LaunchOrReport(Finished->Options);
    }
}
