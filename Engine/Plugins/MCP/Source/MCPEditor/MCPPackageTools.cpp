#include "MCPPackageTools.h"

#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Config/EngineSettings.h"
#include "Containers/StringFormat.h"
#include "Cooker/AssetCooker.h"
#include "Cooker/ProjectPackager.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Log/Log.h"
#include "MCPEditorModule.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Time/PlatformTime.h"

#include <cctype>

namespace Lumina::MCP
{
    namespace MCPPackageToolsInternal
    {
        const char* PackageStageName(EPackageStage Stage)
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

        FString LoweredPackageText(FStringView Text)
        {
            FString Out(Text.GetData(), Text.Len());
            for (char& c : Out)
            {
                c = (char)std::tolower((unsigned char)c);
            }
            return Out;
        }

        // Compiler and build tool phrasing, since "0 error(s)" in a summary line is not a problem.
        bool IsPackageBuildError(FStringView Line)
        {
            const FString Low = LoweredPackageText(Line);
            return Low.Find("error:") != Constants::kIndexNone || Low.Find(" error c") != Constants::kIndexNone
                || Low.Find("fatal error") != Constants::kIndexNone || Low.Find("failed") != Constants::kIndexNone
                || Low.Find("[error]") != Constants::kIndexNone;
        }

        bool IsPackageBuildWarning(FStringView Line)
        {
            const FString Low = LoweredPackageText(Line);
            return Low.Find("warning:") != Constants::kIndexNone || Low.Find(" warning c") != Constants::kIndexNone
                || Low.Find("[warn]") != Constants::kIndexNone;
        }

        bool IsPackageBuildProblem(FStringView Line)
        {
            return IsPackageBuildError(Line) || IsPackageBuildWarning(Line);
        }

        bool IsPackageLogError(FStringView Line)
        {
            return Line.Find("] [error") != Constants::kIndexNone || Line.Find("] [critical") != Constants::kIndexNone
                || Line.Find("] [fatal") != Constants::kIndexNone;
        }

        bool IsPackageLogWarning(FStringView Line)
        {
            return Line.Find("] [warning") != Constants::kIndexNone;
        }

        // Package tools are registered only while the module's server runs, so the module is always there.
        FPackageSession& PackageSession()
        {
            return FMCPEditorModule::GetPackageTools()->Build;
        }

        FPackageGameSession& PackageGame()
        {
            return FMCPEditorModule::GetPackageTools()->Game;
        }

        FString PackageProjectName()
        {
            return Global::GEngine != nullptr ? FString(Global::GEngine->GetProjectName().GetData(), Global::GEngine->GetProjectName().Len()) : FString();
        }

        FString PackageProjectDirectory()
        {
            FString Directory = Global::GEngine != nullptr ? FString(Global::GEngine->GetProjectPath().GetData(), Global::GEngine->GetProjectPath().Len()) : FString();
            while (!Directory.IsEmpty() && (Directory.Last() == '/' || Directory.Last() == '\\'))
            {
                Directory.LeftChopInline(1);
            }
            return Directory;
        }

        FString PackageDefaultOutputDirectory(bool bServer)
        {
            const FString Name = PackageProjectName();
            return PackageProjectDirectory() + "/Build/" + FProjectPackager::GetPackageName(FStringView(*Name, Name.Len()), bServer);
        }

        FString PackageDirectoryOf(const FString& Path)
        {
            const int32 Slash = Path.FindLastOf("/\\");
            return Slash == Constants::kIndexNone ? FString(".") : Path.Mid(0, Slash);
        }

        TVector<FString> TailPackageLog(const TVector<FString>& Lines, int32 From, int32 MaxLines, bool bProblemsOnly, bool (*IsProblem)(FStringView))
        {
            TVector<FString> Picked;
            for (int32 i = From; i < Lines.Num(); ++i)
            {
                if (!bProblemsOnly || IsProblem(FStringView(*Lines[i], Lines[i].Len())))
                {
                    Picked.Add(Lines[i]);
                }
            }
            if (MaxLines > 0 && Picked.Num() > MaxLines)
            {
                Picked.RemoveAt(0, Picked.Num() - MaxLines);
            }
            return Picked;
        }

        bool IsPackageLogProblem(FStringView Line)
        {
            return IsPackageLogError(Line) || IsPackageLogWarning(Line);
        }

        void FillPackageStatus(const SPackageStatusParams& In, SPackageStatusResult& Out)
        {
            FPackageSession& S = PackageSession();
            const EPackageStage Stage = S.Stage.load();

            // A finished worker is joined here, so the next build never meets a live thread.
            if ((Stage == EPackageStage::Succeeded || Stage == EPackageStage::Failed) && S.Worker.joinable())
            {
                S.Worker.join();
            }

            FScopeLock Lock(S.Mutex);
            Out.Stage = PackageStageName(Stage);
            Out.bFinished = Stage == EPackageStage::Succeeded || Stage == EPackageStage::Failed;
            Out.bSuccess = Stage == EPackageStage::Succeeded;
            Out.Error = S.Error;
            Out.Configuration = S.Configuration;
            Out.OutputDirectory = S.OutputDirectory;
            Out.Executable = S.Executable;
            Out.AssetsCooked = S.AssetsCooked;
            Out.Chunks = S.Chunks;
            Out.LogLineCount = S.Lines.Num();
            Out.Errors = S.Errors;
            Out.Warnings = S.Warnings;

            const double End = Out.bFinished ? S.EndSeconds.load() : PlatformTime::Seconds();
            Out.ElapsedSeconds = Stage == EPackageStage::Idle ? 0.0f : (float)(End - S.StartSeconds);
            Out.Log = TailPackageLog(S.Lines, Math::Max(In.FromLine, 0), Math::Max(In.MaxLines, 0), In.bProblemsOnly, &IsPackageBuildProblem);
        }

        // Strips a trailing '\r' so a CRLF line reads the same as an LF one.
        FStringView StripTrailingCR(FStringView Line)
        {
            if (!Line.IsEmpty() && Line.Last() == '\r')
            {
                Line.RemoveSuffix(1);
            }
            return Line;
        }

        // Invokes Fn with each '\n'-terminated line in Text, in order. Returns the consumed prefix length, leaving an unterminated remainder for the caller.
        template <typename FFunc>
        int32 ForEachCompleteLogLine(FStringView Text, FFunc&& Fn)
        {
            int32 Start = 0;
            int32 Consumed = 0;
            while (Start < Text.Len())
            {
                const int32 End = Text.Find('\n', Start);
                if (End == Constants::kIndexNone)
                {
                    break;
                }
                Fn(StripTrailingCR(Text.Mid(Start, End - Start)));
                Start = End + 1;
                Consumed = Start;
            }
            return Consumed;
        }

        // Only the newest part of a long log is read, since a poll wants the tail and the file can reach many megabytes.
        TVector<FString> ReadPackageLogLines(const FString& Path)
        {
            TVector<FString> Lines;
            const FStringView View(*Path, Path.Len());
            if (Path.IsEmpty() || !Filesystem::IsFile(View))
            {
                return Lines;
            }

            constexpr uint64 MaxBytes = Constants::kMiB;
            const uint64 Size = Filesystem::FileSize(View);
            const uint64 Offset = Size > MaxBytes ? Size - MaxBytes : 0;

            TVector<uint8> Bytes;
            if (!Filesystem::ReadFileRange(Bytes, View, Offset, Size - Offset))
            {
                return Lines;
            }

            const FStringView Text(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
            auto AddLine = [&Lines](FStringView Line)
            {
                if (!Line.IsEmpty())
                {
                    Lines.Emplace(Line.GetData(), Line.Len());
                }
            };
            const int32 Consumed = ForEachCompleteLogLine(Text, AddLine);
            if (Consumed < Text.Len())
            {
                AddLine(StripTrailingCR(Text.Mid(Consumed, Text.Len() - Consumed)));
            }

            // A partial first line from the cut is dropped rather than shown half.
            if (Offset > 0 && !Lines.IsEmpty())
            {
                Lines.RemoveAt(0);
            }
            return Lines;
        }

        void FillPackageRunStatus(const SPackageRunParams& In, SPackageRunResult& Out)
        {
            FPackageGameSession& G = PackageGame();
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

            G.ScanNewLogLines();
            Out.Errors = G.Errors;
            Out.Warnings = G.Warnings;

            const TVector<FString> Lines = ReadPackageLogLines(G.LogPath);
            Out.Log = TailPackageLog(Lines, 0, Math::Max(In.MaxLines, 0), In.bProblemsOnly, &IsPackageLogProblem);
        }

        Agent::FToolResult LaunchPackagedGame(const SPackageRunParams& In, SPackageRunResult& Out)
        {
            FString Executable = In.Executable;
            if (Executable.IsEmpty())
            {
                FScopeLock Lock(PackageSession().Mutex);
                Executable = PackageSession().Executable;
            }
            if (Executable.IsEmpty())
            {
                return Agent::FToolResult::Error("No packaged executable yet. Run package.build first, or pass Executable.");
            }
            if (!Filesystem::IsFile(FStringView(*Executable, Executable.Len())))
            {
                return Agent::FToolResult::Error(Lumina::Format("{} does not exist.", Executable));
            }

            FPackageGameSession& G = PackageGame();
            G.Close();

            // A cooked game logs into its data folder, since it never loads a project directory to move the log into.
            const FString Directory = PackageDirectoryOf(Executable);
            FStringView ExeStem(*Executable, Executable.Len());
            ExeStem = ExeStem.Mid(ExeStem.FindLastOf("/\\") + 1);
            if (const int32 Dot = ExeStem.FindLastOf('.'); Dot != Constants::kIndexNone)
            {
                ExeStem = ExeStem.Left(Dot);
            }
            const FString DataDir = Directory + "/" + FString(ExeStem.GetData(), ExeStem.Len()) + "_Data";
            G.LogPath = (Filesystem::Exists(FStringView(*DataDir, DataDir.Len())) ? DataDir : Directory) + "/Logs/Lumina.log";

            // Recorded before the spawn, so ScanNewLogLines can tell a leftover log from this run's own once it changes.
            const Filesystem::FFileStat PreLaunchStat = Filesystem::Stat(FStringView(*G.LogPath, G.LogPath.Len()));
            G.PreLaunchLogSize = PreLaunchStat.IsFile() ? (int64)PreLaunchStat.Size : -1;
            G.PreLaunchLogModifyTime = PreLaunchStat.IsFile() ? PreLaunchStat.LastModifyTime : 0;
            G.bLockedOntoLog = false;
            G.ScannedLogBytes = 0;
            G.Errors = 0;
            G.Warnings = 0;

            G.Process = Platform::SpawnProcess(UTF8_TO_TCHAR(*Executable), UTF8_TO_TCHAR(*In.Arguments), UTF8_TO_TCHAR(*Directory));
            if (!G.Process.IsValid())
            {
                return Agent::FToolResult::Error(Lumina::Format("Could not start {}.", Executable));
            }

            G.Executable = Executable;
            G.StartSeconds = PlatformTime::Seconds();
            G.EndSeconds = 0.0;

            FillPackageRunStatus(In, Out);
            return Agent::FToolResult::Ok(Lumina::Format("Started {} as process {}.", Executable, Out.ProcessId));
        }

        Agent::FToolResult StopPackagedGame(const SPackageRunParams& In, SPackageRunResult& Out)
        {
            PackageGame().Close();
            FillPackageRunStatus(In, Out);
            return Agent::FToolResult::Ok(Lumina::Format("Stopped after {:.0f}s.", Out.ElapsedSeconds));
        }

        Agent::FToolResult ReportPackagedGame(const SPackageRunParams& In, SPackageRunResult& Out)
        {
            FillPackageRunStatus(In, Out);
            return Agent::FToolResult::Ok(Out.bRunning
                ? Lumina::Format("Running for {:.0f}s with {} error(s) and {} warning(s) logged.", Out.ElapsedSeconds, Out.Errors, Out.Warnings)
                : Lumina::Format("Exited with code {} after {:.0f}s, {} error(s) and {} warning(s) logged.", Out.ExitCode, Out.ElapsedSeconds,
                                 Out.Errors, Out.Warnings));
        }

        void RegisterPackageSettings(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageEmptyParams, SPackageSettingsResult>(
                Owner, "package.settings",
                "Report the open project's startup map and cook roots. Assets named in C# string literals, and every asset in a "
                "content folder a script names, are cooked too.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPackageEmptyParams&, SPackageSettingsResult& Out)
                {
                    if (PackageProjectName().IsEmpty())
                    {
                        return Agent::FToolResult::Error("No project is open.");
                    }

                    Out.ProjectName = PackageProjectName();
                    Out.ProjectDirectory = PackageProjectDirectory();
                    Out.DefaultOutputDirectory = PackageDefaultOutputDirectory(false);
                    if (const CProjectSettings* Settings = GetDefault<CProjectSettings>())
                    {
                        const FStringView Map = Settings->GameStartupMap.GetPath();
                        Out.StartupMap = FStringView(Map.GetData(), Map.Len());
                    }
                    for (const FCookRoot& Root : Global::GEngine->GetCookRoots())
                    {
                        SPackageCookRootInfo Info;
                        Info.Asset = Root.Asset;
                        Info.Chunk = Root.Chunk.IsNone() ? FString("Main") : FString(*Root.Chunk.ToString());
                        Out.CookRoots.Add(Info);
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} with {} cook root(s), startup map {}.", Out.ProjectName, Out.CookRoots.Num(),
                                                                 Out.StartupMap.IsEmpty() ? FString("unset") : Out.StartupMap));
                });
        }

        void RegisterPackageBuild(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageBuildParams, SPackageStatusResult>(
                Owner, "package.build",
                "Package the open project as a standalone game: cook to pak files now, then build the executable in the background. "
                "Poll package.status until bFinished.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPackageBuildParams& In, SPackageStatusResult& Out)
                {
                    FPackageSession& S = PackageSession();
                    const EPackageStage Current = S.Stage.load();
                    if (Current == EPackageStage::Cooking || Current == EPackageStage::Building)
                    {
                        return Agent::FToolResult::Error("A package is already in progress. Poll package.status until it finishes.");
                    }
                    if (S.Worker.joinable())
                    {
                        S.Worker.join();
                    }
                    if (PackageProjectName().IsEmpty())
                    {
                        return Agent::FToolResult::Error("No project is open.");
                    }
                    if (In.Configuration != "Shipping" && In.Configuration != "Development" && In.Configuration != "Debug")
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Configuration must be Shipping, Development or Debug, not {}.", In.Configuration));
                    }

                    TOptional<EPackagePlatform> Platform;
                    for (const EPackagePlatform Candidate : { EPackagePlatform::Windows64, EPackagePlatform::Linux64 })
                    {
                        if (FStringView(*In.Platform, In.Platform.Len()) == FPackagePlatformRules::For(Candidate).Name)
                        {
                            Platform = Candidate;
                        }
                    }
                    if (!Platform)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Platform must be Windows64 or Linux64, not {}.", In.Platform));
                    }

                    const FString Name = PackageProjectName();
                    const FString PackageName = FProjectPackager::GetPackageName(FStringView(*Name, Name.Len()), In.bServer);
                    const FString OutputDirectory = In.OutputDirectory.IsEmpty() ? PackageDefaultOutputDirectory(In.bServer) : In.OutputDirectory;
                    if (!Filesystem::MakeDirectoryTree(FStringView(*OutputDirectory, OutputDirectory.Len())))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create {}.", OutputDirectory));
                    }
                    const FString PakPath = FProjectPackager::GetPakPath(FStringView(*OutputDirectory, OutputDirectory.Len()), FStringView(*PackageName, PackageName.Len()));

                    {
                        FScopeLock Lock(S.Mutex);
                        S.Lines.Reset();
                        S.Errors = 0;
                        S.Warnings = 0;
                        S.Error.Reset();
                        S.Configuration = In.Configuration;
                        S.OutputDirectory = OutputDirectory;
                        S.Executable.Reset();
                        S.AssetsCooked = 0;
                        S.Chunks.Reset();
                    }
                    S.StartSeconds = PlatformTime::Seconds();
                    S.Stage.store(EPackageStage::Cooking);

                    FCookOptions CookOptions;
                    CookOptions.bExtractScriptsAsLooseFiles = In.bExtractScriptsAsLooseFiles;
                    const FCookResult Cook = FAssetCooker::Cook(FStringView(*PakPath, PakPath.Len()), CookOptions,
                                                                [&S](FStringView Line) { S.Append(Line); });
                    {
                        FScopeLock Lock(S.Mutex);
                        S.AssetsCooked = (int32)Cook.NumAssetsCooked;
                        for (const FCookChunkResult& Chunk : Cook.Chunks)
                        {
                            SPackageChunkInfo Info;
                            Info.Chunk = FString(*Chunk.Chunk.ToString());
                            Info.PakPath = Chunk.PakPath;
                            Info.Assets = (int32)Chunk.NumAssets;
                            Info.Bytes = (int64)Chunk.Bytes;
                            S.Chunks.Add(Info);
                        }
                    }

                    if (!Cook.bSuccess)
                    {
                        S.Finish(false, FString("Cook failed: ") + Cook.ErrorMessage);
                        FillPackageStatus({}, Out);
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
                        FillPackageStatus({}, Out);
                        return Agent::FToolResult::Ok(Lumina::Format("Cooked {} asset(s) into {} pak file(s) in {}.",
                                                                     Out.AssetsCooked, Out.Chunks.Num(), OutputDirectory));
                    }

                    FPackageBuildOptions Options;
                    Options.OutputDirectory = OutputDirectory;
                    Options.ProjectDirectory = PackageProjectDirectory();
                    Options.BuildConfiguration = In.Configuration;
                    Options.bBuildExecutable = true;
                    Options.bExtractScriptsAsLooseFiles = In.bExtractScriptsAsLooseFiles;
                    Options.bServer = In.bServer;
                    Options.Platform = Platform.GetValue();

                    S.Stage.store(EPackageStage::Building);
                    S.Worker = FThread([&Worker = S, Options, Name, PackageName, PakPath]()
                    {
                        const FPackageBuildResult Result = FProjectPackager::BuildAndCopyOnly(Options, FStringView(*Name, Name.Len()),
                                                                                              FStringView(*PakPath, PakPath.Len()),
                                                                                              [&Worker](FStringView Line) { Worker.Append(Line); });
                        if (Result.bSuccess)
                        {
                            const FString Executable = Options.OutputDirectory + "/" + FPackagePlatformRules::For(Options.Platform).ExecutableFileName(*PackageName);
                            FScopeLock Lock(Worker.Mutex);
                            Worker.Executable = Filesystem::IsFile(FStringView(*Executable, Executable.Len())) ? Executable : FString();
                        }
                        Worker.Finish(Result.bSuccess, Result.ErrorMessage);
                    });

                    FillPackageStatus({}, Out);
                    return Agent::FToolResult::Ok(Lumina::Format("Cooked {} asset(s). Building the {} executable now; poll package.status.",
                                                                 Out.AssetsCooked, In.Configuration));
                });
        }

        void RegisterPackageStatus(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageStatusParams, SPackageStatusResult>(
                Owner, "package.status",
                "Report how the last package.build is going, with its log. Pass the last LogLineCount as FromLine to read only new lines.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SPackageStatusParams& In, SPackageStatusResult& Out)
                {
                    FillPackageStatus(In, Out);
                    if (Out.Stage == "Failed")
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Package failed after {:.0f}s. {}", Out.ElapsedSeconds, Out.Error));
                    }
                    if (Out.Stage == "Succeeded")
                    {
                        return Agent::FToolResult::Ok(Lumina::Format("Packaged in {:.0f}s to {}.", Out.ElapsedSeconds,
                                                                     Out.Executable.IsEmpty() ? Out.OutputDirectory : Out.Executable));
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} for {:.0f}s, {} log line(s).", Out.Stage, Out.ElapsedSeconds, Out.LogLineCount));
                });
        }

        void RegisterPackageRun(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPackageRunParams, SPackageRunResult>(
                Owner, "package.run",
                "Launch a packaged game as its own process, replacing one launched earlier, Stop it, or report its Status, exit "
                "code and log tail. -benchmark -frames=600 in Arguments plays a timed pass and quits.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPackageRunParams& In, SPackageRunResult& Out)
                {
                    if (In.Action == EPackageRunAction::Launch)
                    {
                        return LaunchPackagedGame(In, Out);
                    }
                    if (!PackageGame().Process.IsValid())
                    {
                        return Agent::FToolResult::Error("No packaged game has been launched.");
                    }
                    return In.Action == EPackageRunAction::Stop ? StopPackagedGame(In, Out) : ReportPackagedGame(In, Out);
                });
        }
    }

    using namespace MCPPackageToolsInternal;

    void FPackageSession::Append(FStringView Line)
    {
        FScopeLock Lock(Mutex);
        Lines.Emplace(Line.GetData(), Line.Len());
        const bool bIsWarning = IsPackageBuildWarning(Line);
        Errors += (!bIsWarning && IsPackageBuildError(Line)) ? 1 : 0;
        Warnings += bIsWarning ? 1 : 0;
    }

    void FPackageSession::Finish(bool bSucceeded, const FString& InError)
    {
        {
            FScopeLock Lock(Mutex);
            Error = InError;
        }
        EndSeconds.store(PlatformTime::Seconds());
        Stage.store(bSucceeded ? EPackageStage::Succeeded : EPackageStage::Failed);
    }

    bool FPackageGameSession::Close()
    {
        const bool bWasRunning = Platform::IsProcessRunning(Process);
        Platform::KillProcess(Process);
        if (Process.IsValid() && EndSeconds == 0.0)
        {
            EndSeconds = PlatformTime::Seconds();
        }

        const Platform::FProcessHandle Exited = Process;
        Platform::CloseProcess(Process);
        Process.ProcessId = Exited.ProcessId;
        Process.ExitCode = Exited.ExitCode;
        Process.bExited = Exited.bExited;
        return bWasRunning;
    }

    void FPackageGameSession::ScanNewLogLines()
    {
        const FStringView View(*LogPath, LogPath.Len());
        const Filesystem::FFileStat Stat = LogPath.IsEmpty() ? Filesystem::FFileStat() : Filesystem::Stat(View);
        if (!Stat.IsFile())
        {
            return;
        }

        if (!bLockedOntoLog)
        {
            // The previous run's log can still sit at this path until the game rotates it away; wait for it to visibly change.
            if ((int64)Stat.Size == PreLaunchLogSize && Stat.LastModifyTime == PreLaunchLogModifyTime)
            {
                return;
            }
            bLockedOntoLog = true;
            ScannedLogBytes = 0;
        }
        else if (Stat.Size < ScannedLogBytes)
        {
            // A rotation at the size cap replaces the file mid-run; keep the totals but restart the cursor.
            ScannedLogBytes = 0;
        }

        if (Stat.Size <= ScannedLogBytes)
        {
            return;
        }

        TVector<uint8> Bytes;
        if (!Filesystem::ReadFileRange(Bytes, View, ScannedLogBytes, Stat.Size - ScannedLogBytes))
        {
            return;
        }

        const FStringView Text(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
        ScannedLogBytes += (uint64)ForEachCompleteLogLine(Text, [this](FStringView Line)
        {
            Errors += IsPackageLogError(Line) ? 1 : 0;
            Warnings += IsPackageLogWarning(Line) ? 1 : 0;
        });
    }

    void FPackageToolsState::Shutdown()
    {
        if (Game.Close())
        {
            LOG_WARN("[MCP] Killed the packaged game {} launched by package.run because the editor is quitting.", Game.Executable);
        }

        if (!Build.Worker.joinable())
        {
            return;
        }
        if (Build.Stage.load() == EPackageStage::Building)
        {
            LOG_WARN("[MCP] Waiting for the running package build to finish before the editor quits.");
        }
        Build.Worker.join();
    }

    void RegisterPackageTools(FStringView Owner)
    {
        RegisterPackageSettings(Owner);
        RegisterPackageBuild(Owner);
        RegisterPackageStatus(Owner);
        RegisterPackageRun(Owner);
    }
}
