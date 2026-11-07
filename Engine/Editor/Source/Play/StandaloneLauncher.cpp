#include "StandaloneLauncher.h"

#include "Core/Engine/Engine.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Containers/StringFormat.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        FFixedString GameExecutablePath()
        {
            return Paths::Combine(Paths::GetEngineInstallDirectory(), "Binaries", LUMINA_PLATFORM_NAME, Paths::MakeGameApplicationName());
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

        FFixedString ProjectBinaryPath(const FFixedString& FileName)
        {
            const FString Directory = ProjectDirectory();
            if (Directory.IsEmpty())
            {
                return {};
            }
            return Paths::Combine(Directory, "Binaries", LUMINA_PLATFORM_NAME, FileName);
        }

        // A project with no editor-flavor module has no C++ at all, so a game build would produce nothing.
        bool ProjectHasNativeModule()
        {
            const FFixedString EditorModule = ProjectBinaryPath(Paths::MakeModuleFileName(ProjectName()));
            return !EditorModule.IsEmpty() && Filesystem::Exists(EditorModule);
        }

        FFixedString ProjectGameModulePath()
        {
            return ProjectBinaryPath(Paths::MakeGameModuleFileName(ProjectName()));
        }

        void AppendQuoted(FString& Out, FStringView Value)
        {
            Out += "\"";
            Out += Value;
            Out += "\"";
        }

        FString BuildArguments()
        {
            FString Args = "Build ";
            Args += ProjectName();
            Args += " -TargetType=Game -Configuration=";
            Args += LUMINA_CONFIGURATION_NAME;
            Args += " -Project=";
            AppendQuoted(Args, *ProjectDirectory());
            return Args;
        }

        bool StartGameProcess(const FStandaloneLaunchOptions& Options, FString& OutError)
        {
            const FFixedString Executable = GameExecutablePath();
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

            FString Params = "--Project=";
            AppendQuoted(Params, ProjectFile);

            if (!Options.MapPath.IsEmpty())
            {
                Params += " --map=";
                AppendQuoted(Params, *Options.MapPath);
            }

            // Its own log, or its startup rotates the file this editor is still writing.
            Params += " --logfile=Standalone";

            FString Quoted;
            AppendQuoted(Quoted, *Executable);

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

        PendingOptions = Options;
        bBuildDone.store(false, std::memory_order_relaxed);

        const FString Tool = Paths::GetBuildToolScript();
        const FString Args = BuildArguments();
        const FString WorkingDirectory(Paths::GetEngineInstallDirectory());

        Worker = FThread([this, Tool, Args, WorkingDirectory](const FThreadStop& Stop)
        {
            BuildExitCode = Platform::RunProcessAndWaitCapture(
                UTF8_TO_TCHAR(*Tool),
                UTF8_TO_TCHAR(*Args),
                UTF8_TO_TCHAR(*WorkingDirectory),
                [](FStringView Line)
                {
                    if (!Line.IsEmpty())
                    {
                        LOG_INFO("[Standalone build] {}", *FString(Line.GetData(), Line.Len()));
                    }
                },
                0, &Stop);

            bBuildDone.store(true, std::memory_order_release);
        });
    }

    void FStandaloneLauncher::Tick()
    {
        if (!IsBuilding() || !bBuildDone.load(std::memory_order_acquire))
        {
            return;
        }

        Worker.Join();

        if (BuildExitCode != 0)
        {
            const FString Error = Format("Game build failed (exit code {}). See the log above.", BuildExitCode);
            LOG_ERROR("Standalone: {}", *Error);
            ImGuiX::Notifications::NotifyError("{}", *Error);
            return;
        }

        LaunchOrReport(PendingOptions);
    }

    void FStandaloneLauncher::Shutdown()
    {
        if (!IsBuilding())
        {
            return;
        }

        if (!bBuildDone.load(std::memory_order_acquire))
        {
            LOG_WARN("Standalone: stopping the game build because the editor is shutting down.");
        }

        Worker.RequestStop();
        Worker.Join();
    }
}
