#include "MCPPackageTools.h"

#include "Agent/AgentToolRegistry.h"
#include "Config/EngineSettings.h"
#include "Containers/StringFormat.h"
#include "Cooker/AssetCooker.h"
#include "Cooker/ProjectPackager.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"

#include <atomic>
#include <cctype>

namespace Lumina::MCP
{
    namespace
    {
        enum class EPackageStage : uint8
        {
            Idle,
            Cooking,
            Building,
            Succeeded,
            Failed,
        };

        const char* StageName(EPackageStage Stage)
        {
            switch (Stage)
            {
            case EPackageStage::Cooking:   return "Cooking";
            case EPackageStage::Building:  return "Building";
            case EPackageStage::Succeeded: return "Succeeded";
            case EPackageStage::Failed:    return "Failed";
            default:                       return "Idle";
            }
        }

        FString Lowered(FStringView Text)
        {
            FString Out(Text.data(), Text.size());
            for (char& c : Out)
            {
                c = (char)std::tolower((unsigned char)c);
            }
            return Out;
        }

        // Compiler and build tool phrasing, since "0 error(s)" in a summary line is not a problem.
        bool IsBuildProblem(FStringView Line)
        {
            const FString Low = Lowered(Line);
            return Low.find("error:") != FString::npos || Low.find(" error c") != FString::npos
                || Low.find("fatal error") != FString::npos || Low.find("failed") != FString::npos
                || Low.find("warning:") != FString::npos || Low.find(" warning c") != FString::npos
                || Low.find("[warn]") != FString::npos || Low.find("[error]") != FString::npos;
        }

        bool IsLogError(FStringView Line)
        {
            return Line.find("] [error") != FStringView::npos || Line.find("] [critical") != FStringView::npos
                || Line.find("] [fatal") != FStringView::npos;
        }

        bool IsLogWarning(FStringView Line)
        {
            return Line.find("] [warning") != FStringView::npos;
        }

        struct FPackageSession
        {
            FMutex Mutex;
            TVector<FString> Lines;
            int32 Warnings = 0;

            std::atomic<EPackageStage> Stage{ EPackageStage::Idle };
            FString Error;
            FString Configuration;
            FString OutputDirectory;
            FString Executable;
            int32 AssetsCooked = 0;
            TVector<SPackageChunkInfo> Chunks;
            double StartSeconds = 0.0;
            std::atomic<double> EndSeconds{ 0.0 };

            FThread Worker;

            void Append(FStringView Line)
            {
                FScopeLock Lock(Mutex);
                Lines.emplace_back(Line.data(), Line.size());
                Warnings += IsBuildProblem(Line) ? 1 : 0;
            }

            void Finish(bool bSucceeded, const FString& InError)
            {
                {
                    FScopeLock Lock(Mutex);
                    Error = InError;
                }
                EndSeconds.store(PlatformTime::Seconds());
                Stage.store(bSucceeded ? EPackageStage::Succeeded : EPackageStage::Failed);
            }
        };

        FPackageSession& Session()
        {
            static FPackageSession Instance;
            return Instance;
        }

        struct FGameSession
        {
            Platform::FProcessHandle Process;
            FString Executable;
            FString LogPath;
            double StartSeconds = 0.0;
            double EndSeconds = 0.0;
        };

        FGameSession& Game()
        {
            static FGameSession Instance;
            return Instance;
        }

        FString ProjectName()
        {
            return GEngine != nullptr ? FString(GEngine->GetProjectName().data(), GEngine->GetProjectName().size()) : FString();
        }

        FString ProjectDirectory()
        {
            FString Directory = GEngine != nullptr ? FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()) : FString();
            while (!Directory.empty() && (Directory.back() == '/' || Directory.back() == '\\'))
            {
                Directory.pop_back();
            }
            return Directory;
        }

        FString DefaultOutputDirectory()
        {
            return ProjectDirectory() + "/Build/" + ProjectName();
        }

        FString DirectoryOf(const FString& Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            return Slash == FString::npos ? FString(".") : Path.substr(0, Slash);
        }

        TVector<FString> Tail(const TVector<FString>& Lines, size_t From, size_t MaxLines, bool bProblemsOnly, bool (*IsProblem)(FStringView))
        {
            TVector<FString> Picked;
            for (size_t i = From; i < Lines.size(); ++i)
            {
                if (!bProblemsOnly || IsProblem(FStringView(Lines[i].c_str(), Lines[i].size())))
                {
                    Picked.push_back(Lines[i]);
                }
            }
            if (MaxLines > 0 && Picked.size() > MaxLines)
            {
                Picked.erase(Picked.begin(), Picked.begin() + (Picked.size() - MaxLines));
            }
            return Picked;
        }

        bool IsLogProblem(FStringView Line)
        {
            return IsLogError(Line) || IsLogWarning(Line);
        }

        void FillStatus(const SPackageStatusParams& In, SPackageStatusResult& Out)
        {
            FPackageSession& S = Session();
            const EPackageStage Stage = S.Stage.load();

            // A finished worker is joined here, so the next build never meets a live thread.
            if ((Stage == EPackageStage::Succeeded || Stage == EPackageStage::Failed) && S.Worker.joinable())
            {
                S.Worker.join();
            }

            FScopeLock Lock(S.Mutex);
            Out.Stage = StageName(Stage);
            Out.bFinished = Stage == EPackageStage::Succeeded || Stage == EPackageStage::Failed;
            Out.bSuccess = Stage == EPackageStage::Succeeded;
            Out.Error = S.Error;
            Out.Configuration = S.Configuration;
            Out.OutputDirectory = S.OutputDirectory;
            Out.Executable = S.Executable;
            Out.AssetsCooked = S.AssetsCooked;
            Out.Chunks = S.Chunks;
            Out.LogLineCount = (int32)S.Lines.size();
            Out.Warnings = S.Warnings;

            const double End = Out.bFinished ? S.EndSeconds.load() : PlatformTime::Seconds();
            Out.ElapsedSeconds = Stage == EPackageStage::Idle ? 0.0f : (float)(End - S.StartSeconds);
            Out.Log = Tail(S.Lines, (size_t)Math::Max(In.FromLine, 0), (size_t)Math::Max(In.MaxLines, 0), In.bProblemsOnly, &IsBuildProblem);
        }

        // Only the newest part of a long log is read, since a poll wants the tail and the file can reach many megabytes.
        TVector<FString> ReadLogLines(const FString& Path)
        {
            TVector<FString> Lines;
            const FStringView View(Path.c_str(), Path.size());
            if (Path.empty() || !Filesystem::IsFile(View))
            {
                return Lines;
            }

            constexpr uint64 MaxBytes = 1024 * 1024;
            const uint64 Size = Filesystem::FileSize(View);
            const uint64 Offset = Size > MaxBytes ? Size - MaxBytes : 0;

            TVector<uint8> Bytes;
            if (!Filesystem::ReadFileRange(Bytes, View, Offset, Size - Offset))
            {
                return Lines;
            }

            FStringView Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
            size_t Start = 0;
            while (Start < Text.size())
            {
                size_t End = Text.find('\n', Start);
                if (End == FStringView::npos)
                {
                    End = Text.size();
                }
                FStringView Line = Text.substr(Start, End - Start);
                if (!Line.empty() && Line.back() == '\r')
                {
                    Line.remove_suffix(1);
                }
                if (!Line.empty())
                {
                    Lines.emplace_back(Line.data(), Line.size());
                }
                Start = End + 1;
            }

            // A partial first line from the cut is dropped rather than shown half.
            if (Offset > 0 && !Lines.empty())
            {
                Lines.erase(Lines.begin());
            }
            return Lines;
        }

        void RegisterSettings(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageEmptyParams, SPackageSettingsResult>(
                Owner, "package.settings",
                "Report what a package of the open project would contain: its startup map and the cook roots the cooker walks from. "
                "Asset references in C# string literals and every asset in a content folder a script names are cooked too.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPackageEmptyParams&, SPackageSettingsResult& Out)
                {
                    if (ProjectName().empty())
                    {
                        return Agent::FToolResult::Error("No project is open.");
                    }

                    Out.ProjectName = ProjectName();
                    Out.ProjectDirectory = ProjectDirectory();
                    Out.DefaultOutputDirectory = DefaultOutputDirectory();
                    if (const CProjectSettings* Settings = GetDefault<CProjectSettings>())
                    {
                        const FStringView Map = Settings->GameStartupMap.GetPath();
                        Out.StartupMap.assign(Map.data(), Map.size());
                    }
                    for (const FCookRoot& Root : GEngine->GetCookRoots())
                    {
                        SPackageCookRootInfo Info;
                        Info.Asset = Root.Asset;
                        Info.Chunk = Root.Chunk.IsNone() ? FString("Main") : FString(Root.Chunk.ToString().c_str());
                        Out.CookRoots.push_back(Info);
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} with {} cook root(s), startup map {}.", Out.ProjectName, Out.CookRoots.size(),
                                                                 Out.StartupMap.empty() ? FString("unset") : Out.StartupMap));
                });
        }

        void RegisterBuild(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageBuildParams, SPackageStatusResult>(
                Owner, "package.build",
                "Package the open project as a standalone game. Cooks its content into pak files right away, then builds and copies the "
                "executable in the background, so poll package.status until bFinished. Set bBuildExecutable false to cook only.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPackageBuildParams& In, SPackageStatusResult& Out)
                {
                    FPackageSession& S = Session();
                    const EPackageStage Current = S.Stage.load();
                    if (Current == EPackageStage::Cooking || Current == EPackageStage::Building)
                    {
                        return Agent::FToolResult::Error("A package is already in progress. Poll package.status until it finishes.");
                    }
                    if (S.Worker.joinable())
                    {
                        S.Worker.join();
                    }
                    if (ProjectName().empty())
                    {
                        return Agent::FToolResult::Error("No project is open.");
                    }
                    if (In.Configuration != "Shipping" && In.Configuration != "Development" && In.Configuration != "Debug")
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Configuration must be Shipping, Development or Debug, not {}.", In.Configuration));
                    }

                    const FString Name = ProjectName();
                    const FString OutputDirectory = In.OutputDirectory.empty() ? DefaultOutputDirectory() : In.OutputDirectory;
                    if (!Filesystem::MakeDirectoryTree(FStringView(OutputDirectory.c_str(), OutputDirectory.size())))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create {}.", OutputDirectory));
                    }
                    const FString PakPath = FProjectPackager::GetPakPath(FStringView(OutputDirectory.c_str(), OutputDirectory.size()), FStringView(Name.c_str(), Name.size()));

                    {
                        FScopeLock Lock(S.Mutex);
                        S.Lines.clear();
                        S.Warnings = 0;
                        S.Error.clear();
                        S.Configuration = In.Configuration;
                        S.OutputDirectory = OutputDirectory;
                        S.Executable.clear();
                        S.AssetsCooked = 0;
                        S.Chunks.clear();
                    }
                    S.StartSeconds = PlatformTime::Seconds();
                    S.Stage.store(EPackageStage::Cooking);

                    FCookOptions CookOptions;
                    CookOptions.bExtractScriptsAsLooseFiles = In.bExtractScriptsAsLooseFiles;
                    const FCookResult Cook = FAssetCooker::Cook(FStringView(PakPath.c_str(), PakPath.size()), CookOptions,
                                                                [&S](FStringView Line) { S.Append(Line); });
                    {
                        FScopeLock Lock(S.Mutex);
                        S.AssetsCooked = (int32)Cook.NumAssetsCooked;
                        for (const FCookChunkResult& Chunk : Cook.Chunks)
                        {
                            SPackageChunkInfo Info;
                            Info.Chunk = FString(Chunk.Chunk.ToString().c_str());
                            Info.PakPath = Chunk.PakPath;
                            Info.Assets = (int32)Chunk.NumAssets;
                            Info.Bytes = (int64)Chunk.Bytes;
                            S.Chunks.push_back(Info);
                        }
                    }

                    if (!Cook.bSuccess)
                    {
                        S.Finish(false, FString("Cook failed: ") + Cook.ErrorMessage);
                        FillStatus({}, Out);
                        return Agent::FToolResult::Error(Out.Error);
                    }

                    // Walks the VFS, so it stays here on the game thread rather than going to the worker.
                    if (In.bExtractScriptsAsLooseFiles)
                    {
                        FProjectPackager::ExtractLooseScripts(OutputDirectory, [&S](FStringView Line) { S.Append(Line); });
                    }

                    if (!In.bBuildExecutable)
                    {
                        S.Finish(true, FString());
                        FillStatus({}, Out);
                        return Agent::FToolResult::Ok(Lumina::Format("Cooked {} asset(s) into {} pak file(s) in {}.",
                                                                     Out.AssetsCooked, Out.Chunks.size(), OutputDirectory));
                    }

                    FPackageBuildOptions Options;
                    Options.OutputDirectory = OutputDirectory;
                    Options.ProjectDirectory = ProjectDirectory();
                    Options.BuildConfiguration = In.Configuration;
                    Options.bBuildExecutable = true;
                    Options.bExtractScriptsAsLooseFiles = In.bExtractScriptsAsLooseFiles;

                    S.Stage.store(EPackageStage::Building);
                    S.Worker = FThread([Options, Name, PakPath]()
                    {
                        FPackageSession& Worker = Session();
                        const FPackageBuildResult Result = FProjectPackager::BuildAndCopyOnly(Options, FStringView(Name.c_str(), Name.size()),
                                                                                              FStringView(PakPath.c_str(), PakPath.size()),
                                                                                              [&Worker](FStringView Line) { Worker.Append(Line); });
                        if (Result.bSuccess)
                        {
                            const FString Executable = Options.OutputDirectory + "/" + Name + ".exe";
                            FScopeLock Lock(Worker.Mutex);
                            Worker.Executable = Filesystem::IsFile(FStringView(Executable.c_str(), Executable.size())) ? Executable : FString();
                        }
                        Worker.Finish(Result.bSuccess, Result.ErrorMessage);
                    });

                    FillStatus({}, Out);
                    return Agent::FToolResult::Ok(Lumina::Format("Cooked {} asset(s). Building the {} executable now; poll package.status.",
                                                                 Out.AssetsCooked, In.Configuration));
                });
        }

        void RegisterStatus(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageStatusParams, SPackageStatusResult>(
                Owner, "package.status",
                "Report how the last package.build is going, with its log. Pass FromLine as the last LogLineCount to read only new lines.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPackageStatusParams& In, SPackageStatusResult& Out)
                {
                    FillStatus(In, Out);
                    if (Out.Stage == "Failed")
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Package failed after {:.0f}s. {}", Out.ElapsedSeconds, Out.Error));
                    }
                    if (Out.Stage == "Succeeded")
                    {
                        return Agent::FToolResult::Ok(Lumina::Format("Packaged in {:.0f}s to {}.", Out.ElapsedSeconds,
                                                                     Out.Executable.empty() ? Out.OutputDirectory : Out.Executable));
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} for {:.0f}s, {} log line(s).", Out.Stage, Out.ElapsedSeconds, Out.LogLineCount));
                });
        }

        void FillGameStatus(const SPackageGameStatusParams& In, SPackageGameStatusResult& Out)
        {
            FGameSession& G = Game();
            Out.bRunning = Platform::IsProcessRunning(G.Process);
            if (!Out.bRunning && G.Process.IsValid() && G.EndSeconds == 0.0)
            {
                G.EndSeconds = PlatformTime::Seconds();
            }
            Out.ProcessId = (int32)G.Process.ProcessId;
            Out.ExitCode = G.Process.ExitCode;
            Out.Executable = G.Executable;
            Out.LogPath = G.LogPath;
            Out.ElapsedSeconds = G.Process.IsValid() ? (float)((Out.bRunning ? PlatformTime::Seconds() : G.EndSeconds) - G.StartSeconds) : 0.0f;

            const TVector<FString> Lines = ReadLogLines(G.LogPath);
            for (const FString& Line : Lines)
            {
                const FStringView View(Line.c_str(), Line.size());
                Out.Errors += IsLogError(View) ? 1 : 0;
                Out.Warnings += IsLogWarning(View) ? 1 : 0;
            }
            Out.Log = Tail(Lines, 0, (size_t)Math::Max(In.MaxLines, 0), In.bProblemsOnly, &IsLogProblem);
        }

        void RegisterLaunch(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageLaunchParams, SPackageGameStatusResult>(
                Owner, "package.launch",
                "Run a packaged game as its own process, stopping one this tool started earlier. Watch it with package.game_status and end it "
                "with package.stop. Arguments such as -benchmark -frames=600 make it play a timed pass and quit by itself.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPackageLaunchParams& In, SPackageGameStatusResult& Out)
                {
                    FString Executable = In.Executable;
                    if (Executable.empty())
                    {
                        FScopeLock Lock(Session().Mutex);
                        Executable = Session().Executable;
                    }
                    if (Executable.empty())
                    {
                        return Agent::FToolResult::Error("No packaged executable yet. Run package.build first, or pass Executable.");
                    }
                    if (!Filesystem::IsFile(FStringView(Executable.c_str(), Executable.size())))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} does not exist.", Executable));
                    }

                    FGameSession& G = Game();
                    Platform::KillProcess(G.Process);
                    Platform::CloseProcess(G.Process);

                    const FString Directory = DirectoryOf(Executable);
                    G.Process = Platform::SpawnProcess(UTF8_TO_TCHAR(Executable.c_str()), UTF8_TO_TCHAR(In.Arguments.c_str()), UTF8_TO_TCHAR(Directory.c_str()));
                    if (!G.Process.IsValid())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not start {}.", Executable));
                    }

                    // A cooked game logs into its data folder, since it never loads a project directory to move the log into.
                    G.Executable = Executable;
                    FStringView ExeStem(Executable.c_str(), Executable.size());
                    ExeStem = ExeStem.substr(ExeStem.find_last_of("/\\") + 1);
                    ExeStem = ExeStem.substr(0, ExeStem.find_last_of('.'));
                    const FString DataDir = Directory + "/" + FString(ExeStem.data(), ExeStem.size()) + "_Data";
                    G.LogPath = (Filesystem::Exists(FStringView(DataDir.c_str(), DataDir.size())) ? DataDir : Directory) + "/Logs/Lumina.log";
                    G.StartSeconds = PlatformTime::Seconds();
                    G.EndSeconds = 0.0;

                    FillGameStatus({}, Out);
                    return Agent::FToolResult::Ok(Lumina::Format("Started {} as process {}.", Executable, Out.ProcessId));
                });
        }

        void RegisterGameStatus(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageGameStatusParams, SPackageGameStatusResult>(
                Owner, "package.game_status",
                "Report whether the game package.launch started is still running, how it exited, and the tail of its log with error and warning counts.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPackageGameStatusParams& In, SPackageGameStatusResult& Out)
                {
                    if (!Game().Process.IsValid())
                    {
                        return Agent::FToolResult::Error("No packaged game has been launched.");
                    }
                    FillGameStatus(In, Out);
                    return Agent::FToolResult::Ok(Out.bRunning
                        ? Lumina::Format("Running for {:.0f}s with {} error(s) and {} warning(s) logged.", Out.ElapsedSeconds, Out.Errors, Out.Warnings)
                        : Lumina::Format("Exited with code {} after {:.0f}s, {} error(s) and {} warning(s) logged.", Out.ExitCode, Out.ElapsedSeconds,
                                         Out.Errors, Out.Warnings));
                });
        }

        void RegisterStop(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageGameStatusParams, SPackageGameStatusResult>(
                Owner, "package.stop",
                "End the game package.launch started.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPackageGameStatusParams& In, SPackageGameStatusResult& Out)
                {
                    if (!Game().Process.IsValid())
                    {
                        return Agent::FToolResult::Error("No packaged game has been launched.");
                    }
                    Platform::KillProcess(Game().Process);
                    FillGameStatus(In, Out);
                    return Agent::FToolResult::Ok(Lumina::Format("Stopped after {:.0f}s.", Out.ElapsedSeconds));
                });
        }
    }

    void RegisterPackageTools(FStringView Owner)
    {
        RegisterSettings(Owner);
        RegisterBuild(Owner);
        RegisterStatus(Owner);
        RegisterLaunch(Owner);
        RegisterGameStatus(Owner);
        RegisterStop(Owner);
    }
}
