#include "ProjectPackager.h"

#include <filesystem>

#include "AssetCooker.h"
#include "Core/Engine/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"

#include <fstream>

namespace Lumina
{
    namespace
    {
        void Log(const TFunction<void(FStringView)>& LogFunc, FStringView Msg)
        {
            if (LogFunc)
            {
                LogFunc(Msg);
            }
            LOG_INFO("[Packager] {}", FString(Msg.data(), Msg.size()).c_str());
        }

        bool CopyFileTo(const std::filesystem::path& Src, const std::filesystem::path& Dst)
        {
            std::error_code Ec;
            std::filesystem::create_directories(Dst.parent_path(), Ec);
            std::filesystem::copy_file(Src, Dst, std::filesystem::copy_options::overwrite_existing, Ec);
            return !Ec;
        }

        bool EndsWithCI(FStringView Vp, FStringView Suffix)
        {
            if (Vp.size() < Suffix.size())
            {
                return false;
            }
            FStringView Tail = Vp.substr(Vp.size() - Suffix.size());
            for (size_t i = 0; i < Suffix.size(); ++i)
            {
                char a = Tail[i]; char b = Suffix[i];
                if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        }

        // Mirror every non-.lasset /Game/ file under <OutDir>/Game/ for loose-files mode.
        size_t CopyLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc)
        {
            std::error_code Ec;
            size_t Count = 0;

            // Mirror the project root's loose (non-.lasset) files under <OutDir>/<TopName>/, preserving the
            // top-level dir name so they re-resolve under the same alias at runtime. /Game is the project
            // root, so this covers loose content (Content/.rml/.rcss/...) and C# scripts (Scripts/) alike.
            struct FLooseRoot { const char* Alias; const char* Top; };
            const FLooseRoot Roots[] = { { "/Game", "Game" } };

            for (const FLooseRoot& Root : Roots)
            {
                const std::filesystem::path DstRoot = std::filesystem::path(OutDir.c_str()) / Root.Top;
                std::filesystem::create_directories(DstRoot, Ec);

                const FString Alias(Root.Alias);
                const FString Prefix = Alias + "/";

                VFS::RecursiveDirectoryIterator(FStringView(Alias.c_str(), Alias.size()), [&](const VFS::FFileInfo& Info)
                {
                    if (Info.IsDirectory())
                    {
                        return;
                    }

                    FStringView Vp(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                    if (EndsWithCI(Vp, ".lasset"))
                    {
                        return;
                    }

                    // C# build artifacts (generated project, obj/bin output) are never needed at
                    // runtime, the C# host compiles .cs sources directly, so keep them out of the package.
                    if (EndsWithCI(Vp, ".csproj")
                        || Vp.find("/obj/") != FStringView::npos
                        || Vp.find("/bin/") != FStringView::npos)
                    {
                        return;
                    }

                    if (Vp.size() <= Prefix.size())
                    {
                        return;
                    }
                    FStringView Relative = Vp.substr(Prefix.size());

                    TVector<uint8> Bytes;
                    if (!VFS::ReadFile(Bytes, Info.VirtualPath))
                    {
                        return;
                    }

                    std::filesystem::path Dst = DstRoot / std::string(Relative.data(), Relative.size());
                    std::filesystem::create_directories(Dst.parent_path(), Ec);

                    std::ofstream Out(Dst, std::ios::binary);
                    if (!Out)
                    {
                        return;
                    }
                    Out.write(reinterpret_cast<const char*>(Bytes.data()), (std::streamsize)Bytes.size());

                    ++Count;
                    if (LogFunc)
                    {
                        LogFunc(FString().sprintf("  + %s/%.*s",
                            Root.Top, (int)Relative.size(), Relative.data()).c_str());
                    }
                });
            }
            return Count;
        }

        // Stem ends with "-<OtherConfig>"; used to skip wrong-config DLLs from a previous Editor build.
        bool HasOtherConfigSuffix(const std::string& Stem, const FString& MyConfig)
        {
            const char* Configs[] = { "Debug", "Development", "Shipping" };
            for (const char* Cfg : Configs)
            {
                if (MyConfig == Cfg)
                {
                    continue;
                }
                const std::string Suffix = std::string("-") + Cfg;
                if (Stem.size() >= Suffix.size() &&
                    Stem.compare(Stem.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        // True if SourceDir has a "<Stem>-<Config>.dll" sibling; identifies stale pre-targetsuffix dupes.
        bool HasSuffixedSibling(const std::filesystem::path& SourceDir, const std::string& Stem)
        {
            std::error_code Ec;
            for (const char* Cfg : { "Debug", "Development", "Shipping" })
            {
                std::filesystem::path Sibling = SourceDir / (Stem + "-" + Cfg + ".dll");
                if (std::filesystem::exists(Sibling, Ec))
                {
                    return true;
                }
            }
            return false;
        }

        // Editor-only / tooling DLLs that aren't needed at runtime; hard-coded since no programmatic check exists.
        bool IsEditorOnlyDll(const std::string& FileName)
        {
            // libclang.dll is for the Reflector tool's Clang frontend (compile-time only).
            return FileName == "libclang.dll";
        }

        size_t CopyRuntimePayload(const std::filesystem::path& SourceDir,
                                  const std::filesystem::path& DestDir,
                                  const FString& ConfigSuffix,
                                  FStringView ProjectName,
                                  const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Copied = 0;
            size_t Skipped = 0;
            const std::string MyExeName = std::string("Lumina-") + ConfigSuffix.c_str() + ".exe";

            std::error_code Ec;
            for (const auto& Entry : std::filesystem::directory_iterator(SourceDir, Ec))
            {
                if (!Entry.is_regular_file())
                {
                    continue;
                }

                const std::string FileName = Entry.path().filename().string();
                const std::string Ext      = Entry.path().extension().string();
                const std::string Stem     = Entry.path().stem().string();

                // Skip tools / wrong-config exes.
                if (Ext == ".exe" && FileName != MyExeName)
                {
                    continue;
                }

                if (Ext != ".dll" && Ext != ".exe")
                {
                    continue;
                }

                if (HasOtherConfigSuffix(Stem, ConfigSuffix))
                {
                    ++Skipped;
                    continue;
                }

                // Skip stale Editor-*.dll left over from a prior Editor build.
                if (Stem.size() >= 7 && Stem.compare(0, 7, "Editor-") == 0)
                {
                    ++Skipped;
                    continue;
                }

                if (IsEditorOnlyDll(FileName))
                {
                    ++Skipped;
                    continue;
                }

                // Stale unsuffixed dupe of a now-suffixed DLL (pre-targetsuffix builds).
                if (Ext == ".dll"
                    && Stem.find("-Debug") == std::string::npos
                    && Stem.find("-Development") == std::string::npos
                    && Stem.find("-Shipping") == std::string::npos
                    && HasSuffixedSibling(SourceDir, Stem))
                {
                    ++Skipped;
                    continue;
                }

                // Rename launcher to <ProjectName>.exe; safe because it never reads its own filename.
                std::filesystem::path DstName = Entry.path().filename();
                if (Ext == ".exe" && !ProjectName.empty())
                {
                    DstName = std::filesystem::path(
                        FString().sprintf("%.*s.exe", (int)ProjectName.size(), ProjectName.data()).c_str());
                }

                std::filesystem::path Dst = DestDir / DstName;
                if (CopyFileTo(Entry.path(), Dst))
                {
                    ++Copied;
                    Log(LogFunc, FString().sprintf("  + %s -> %s",
                        FileName.c_str(), DstName.string().c_str()).c_str());
                }
                else
                {
                    Log(LogFunc, FString().sprintf("  [warn] failed to copy %s", FileName.c_str()).c_str());
                }
            }

            if (Skipped > 0)
            {
                Log(LogFunc, FString().sprintf("  (skipped %zu wrong-config / editor-only / stale files)", Skipped).c_str());
            }
            return Copied;
        }
    }

    FPackageBuildResult FProjectPackager::BuildAndCopyOnly(
        const FPackageBuildOptions& Options,
        FStringView ProjectName,
        FStringView PakPath,
        const TFunction<void(FStringView)>& LogFunc)
    {
        FPackageBuildResult Result;
        Result.OutputDirectory = Options.OutputDirectory;
        Result.PakPath.assign(PakPath.data(), PakPath.size());

        const FString MSBuild = Options.MSBuildPath.empty() ? DefaultMSBuildPath() : Options.MSBuildPath;
        const FString Config  = Options.MSBuildConfiguration.empty() ? FString("Shipping") : Options.MSBuildConfiguration;

        const FString SolutionPath = FString(Paths::GetEngineInstallDirectory().c_str()) + "/Lumina.slnx";

        FString Args;
        Args = FString("\"") + SolutionPath + "\""
            + " -p:Configuration=" + Config
            + " -p:Platform=Game"
            + " -m -v:minimal -nologo";

        Log(LogFunc, FString().sprintf("Running MSBuild: %s %s", MSBuild.c_str(), Args.c_str()).c_str());

        const std::wstring MSBuildW(MSBuild.begin(), MSBuild.end());
        const std::wstring ArgsW(Args.begin(), Args.end());
        const std::wstring CwdW(Paths::GetEngineInstallDirectory().c_str(),
            Paths::GetEngineInstallDirectory().c_str() + Paths::GetEngineInstallDirectory().size());
        
        const int ExitCode = Platform::RunProcessAndWaitCapture(
            MSBuildW.c_str(), ArgsW.c_str(), CwdW.c_str(),
            [&LogFunc](FStringView Line)
            {
                if (LogFunc && !Line.empty())
                {
                    FString Prefixed = FString("  | ");
                    Prefixed.append(Line.data(), Line.size());
                    LogFunc(Prefixed);
                }
            });

        if (ExitCode != 0)
        {
            Result.ErrorMessage = FString().sprintf(
                "MSBuild failed (exit code %d). See log above for the build error. Cooked PAK is still at %.*s.",
                ExitCode, (int)PakPath.size(), PakPath.data());
            return Result;
        }

        const std::filesystem::path BinariesDir =
            std::filesystem::path(Paths::GetEngineInstallDirectory().c_str()) / "Binaries" / "Windows64";
        const std::filesystem::path DestDir(Options.OutputDirectory.c_str());

        Log(LogFunc, FString().sprintf("Copying %s binaries from %s",
            Config.c_str(), BinariesDir.string().c_str()).c_str());

        const size_t Copied = CopyRuntimePayload(BinariesDir, DestDir, Config, ProjectName, LogFunc);
        if (Copied == 0)
        {
            Result.ErrorMessage = "MSBuild reported success but no matching binaries were found to copy. Check the build output.";
            return Result;
        }
        Log(LogFunc, FString().sprintf("Copied %zu runtime files.", Copied).c_str());

        Result.bSuccess = true;
        return Result;
    }

    size_t FProjectPackager::ExtractLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc)
    {
        return CopyLooseScripts(OutDir, LogFunc);
    }

    FString FProjectPackager::DefaultMSBuildPath()
    {
        // Cache; vswhere subprocess isn't free and the answer doesn't change in-session.
        static FString CachedPath;
        if (!CachedPath.empty())
        {
            return CachedPath;
        }

        // vswhere.exe is at a known path with every VS 2017+ install; canonical Microsoft recipe.
        // -find matches x86 + amd64 MSBuild.exe; take the first line (x86 builds fine).
        const char* VsWhereCandidates[] =
        {
            R"(C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe)",
            R"(C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe)",
        };

        for (const char* Candidate : VsWhereCandidates)
        {
            std::error_code Ec;
            if (!std::filesystem::exists(Candidate, Ec))
            {
                continue;
            }

            FString FirstLine;
            const std::string CandidateStr(Candidate);
            const std::wstring VsWhereW(CandidateStr.begin(), CandidateStr.end());
            const std::wstring ArgsW = LR"(-latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" -nologo)";

            const int ExitCode = Platform::RunProcessAndWaitCapture(
                VsWhereW.c_str(), ArgsW.c_str(), nullptr,
                [&FirstLine](FStringView Line)
                {
                    // vswhere can emit multiple matches; first usable wins.
                    if (FirstLine.empty() && !Line.empty())
                    {
                        FirstLine.assign(Line.data(), Line.size());
                    }
                });

            if (ExitCode == 0 && !FirstLine.empty())
            {
                // Normalize Windows backslashes to forward slashes.
                std::replace(FirstLine.begin(), FirstLine.end(), '\\', '/');
                CachedPath = Move(FirstLine);
                return CachedPath;
            }
        }

        // Fallback: VS 2026 Community default; user can override via "MSBuild Path".
        CachedPath = "C:/Program Files/Microsoft Visual Studio/2026/Community/MSBuild/Current/Bin/MSBuild.exe";
        return CachedPath;
    }

    FPackageBuildResult FProjectPackager::Package(const FPackageBuildOptions& Options, const TFunction<void(FStringView)>& LogFunc)
    {
        FPackageBuildResult Result;

        if (GEngine == nullptr || GEngine->GetProjectName().empty())
        {
            Result.ErrorMessage = "No project loaded.";
            return Result;
        }

        const FString ProjectName(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());

        FString OutDir = Options.OutputDirectory;
        if (OutDir.empty())
        {
            OutDir = FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()) + "/Build/" + ProjectName;
        }

        std::error_code Ec;
        std::filesystem::create_directories(OutDir.c_str(), Ec);
        if (Ec)
        {
            Result.ErrorMessage = FString("Failed to create output directory: ") + OutDir + " (" + Ec.message().c_str() + ")";
            return Result;
        }
        Result.OutputDirectory = OutDir;

        Log(LogFunc, FString().sprintf("Output directory: %s", OutDir.c_str()).c_str());

        const FString PakPath = OutDir + "/" + ProjectName + ".pak";
        Log(LogFunc, FString().sprintf("Cooking PAK: %s", PakPath.c_str()).c_str());

        FCookOptions CookOpts;
        CookOpts.bExtractScriptsAsLooseFiles = Options.bExtractScriptsAsLooseFiles;
        CookOpts.ExtraFiles                  = Options.ExtraFiles;
        CookOpts.ExtraDirectories            = Options.ExtraDirectories;

        const FCookResult Cook = FAssetCooker::Cook(PakPath, CookOpts, LogFunc);
        if (!Cook.bSuccess)
        {
            Result.ErrorMessage = FString("Cook failed: ") + Cook.ErrorMessage;
            return Result;
        }
        
        Result.PakPath = PakPath;
        Log(LogFunc, FString().sprintf("Cook OK: %zu assets, %zu extras, %zu bytes", Cook.NumAssetsCooked, Cook.NumExtraFiles, Cook.TotalBytes).c_str());

        if (Options.bExtractScriptsAsLooseFiles)
        {
            Log(LogFunc, "Extracting loose /Game files...");
            const size_t Extracted = CopyLooseScripts(OutDir, LogFunc);
            Log(LogFunc, FString().sprintf("Extracted %zu loose script files.", Extracted).c_str());
        }

        if (Options.bBuildExecutable)
        {
            FPackageBuildOptions LocalOpts = Options;
            LocalOpts.OutputDirectory = OutDir;
            const FPackageBuildResult Sub = BuildAndCopyOnly(LocalOpts, ProjectName, PakPath, LogFunc);
            if (!Sub.bSuccess)
            {
                Result.ErrorMessage = Sub.ErrorMessage;
                return Result;
            }
        }
        else
        {
            Log(LogFunc, "Skipped executable build (cook only).");
        }

        Result.bSuccess = true;
        Log(LogFunc, "Package complete.");
        return Result;
    }
}
