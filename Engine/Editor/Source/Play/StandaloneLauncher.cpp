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

        FString Join(FStringView Left, FStringView Right)
        {
            FString Result(Left.data(), Left.size());
            if (!Result.empty() && Result.back() != '/' && Result.back() != '\\')
            {
                Result.push_back('/');
            }
            Result.append(Right.data(), Right.size());
            return Result;
        }

        FString EngineBinariesDirectory()
        {
            return Join(Paths::GetEngineInstallDirectory(), Join("Binaries", LUMINA_PLATFORM_NAME));
        }

        FString GameExecutablePath()
        {
            const FFixedString Name = Paths::MakeGameApplicationName();
            return Join(EngineBinariesDirectory(), FStringView(Name.c_str(), Name.size()));
        }

        FString ProjectDirectory()
        {
            const FStringView Path = GEngine != nullptr ? GEngine->GetProjectPath() : FStringView();
            return FString(Path.data(), Path.size());
        }

        FString ProjectName()
        {
            const FStringView Name = GEngine != nullptr ? GEngine->GetProjectName() : FStringView();
            return FString(Name.data(), Name.size());
        }

        FString ProjectFilePath()
        {
            const FString Directory = ProjectDirectory();
            if (Directory.empty())
            {
                return {};
            }

            FString Candidate = Join(Directory, ProjectName() + ".lproject");
            if (Filesystem::Exists(Candidate))
            {
                return Candidate;
            }

            FString Found;
            Filesystem::IterateDirectory(Directory, [&Found](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Found.empty() || Entry.IsDirectory() || Entry.GetExtension() != FStringView(".lproject"))
                {
                    return;
                }
                Found.assign(Entry.FullPath.data(), Entry.FullPath.size());
            });
            return Found;
        }

        FString ProjectBinaryPath(const FFixedString& FileName)
        {
            const FString Directory = ProjectDirectory();
            if (Directory.empty())
            {
                return {};
            }
            return Join(Join(Directory, Join("Binaries", LUMINA_PLATFORM_NAME)),
                        FStringView(FileName.c_str(), FileName.size()));
        }

        // A project with no editor-flavor module has no C++ at all, so a game build would produce nothing.
        bool ProjectHasNativeModule()
        {
            const FString EditorModule = ProjectBinaryPath(Paths::MakeModuleFileName(ProjectName()));
            return !EditorModule.empty() && Filesystem::Exists(EditorModule);
        }

        FString ProjectGameModulePath()
        {
            return ProjectBinaryPath(Paths::MakeGameModuleFileName(ProjectName()));
        }

        FString BuildToolPath()
        {
        #if defined(LE_PLATFORM_WINDOWS)
            return Join(Paths::GetEngineInstallDirectory(), "LuminaBuild.bat");
        #else
            return Join(Paths::GetEngineInstallDirectory(), "LuminaBuild.sh");
        #endif
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
                                  Executable.c_str());
                return false;
            }

            const FString ProjectFile = ProjectFilePath();
            if (ProjectFile.empty())
            {
                OutError = "No .lproject file for the loaded project, so the game has nothing to open.";
                return false;
            }

            FString Params = "--Project=\"";
            Params += ProjectFile;
            Params += "\"";

            if (!Options.MapPath.empty())
            {
                Params += " --map=";
                Params += Options.MapPath;
            }

            // Its own log, or its startup rotates the file this editor is still writing.
            Params += " --logfile=Standalone";

            FString Quoted = "\"";
            Quoted += Executable;
            Quoted += "\"";

            const int Error = Platform::LaunchProcess(UTF8_TO_TCHAR(Quoted.c_str()), UTF8_TO_TCHAR(Params.c_str()));
            if (Error != 0)
            {
                OutError = Format("Could not start {} (system error {}).", Executable.c_str(), Error);
                return false;
            }

            LOG_DISPLAY("Standalone: launched {} {}", Executable.c_str(), Params.c_str());
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

            LOG_ERROR("Standalone: {}", Error.c_str());
            ImGuiX::Notifications::NotifyError("{}", Error.c_str());
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
        return BuildToolPath() + " " + BuildArguments();
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
            LOG_ERROR("Standalone: the project's Game binaries are missing. Run: {}", Command.c_str());
            ImGuiX::Notifications::NotifyError("Game binaries are missing. Run: {}", Command.c_str());
            return;
        }

        ImGuiX::Notifications::NotifyInfo("Building the Game target for standalone play.");

        GSession = MakeShared<FLaunchSession>();
        GSession->Options = Options;

        TSharedPtr<FLaunchSession> Session = GSession;
        const FString Tool = BuildToolPath();
        const FString Args = BuildArguments();
        const FString WorkingDirectory(Paths::GetEngineInstallDirectory());

        GWorker = FThread([Session, Tool, Args, WorkingDirectory]()
        {
            const int ExitCode = Platform::RunProcessAndWaitCapture(
                UTF8_TO_TCHAR(Tool.c_str()),
                UTF8_TO_TCHAR(Args.c_str()),
                UTF8_TO_TCHAR(WorkingDirectory.c_str()),
                [](FStringView Line)
                {
                    if (!Line.empty())
                    {
                        LOG_INFO("[Standalone build] {}", FString(Line.data(), Line.size()).c_str());
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
            LOG_ERROR("Standalone: {}", Finished->ErrorMessage.c_str());
            ImGuiX::Notifications::NotifyError("{}", Finished->ErrorMessage.c_str());
            return;
        }

        LaunchOrReport(Finished->Options);
    }
}
