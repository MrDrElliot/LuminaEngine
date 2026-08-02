#include "ProjectPackager.h"

#include <filesystem>

#include "AssetCooker.h"
#include "Core/Engine/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/DotNet/DotNetHost.h"

#include <fstream>

namespace Lumina
{
    namespace
    {
        void LogPackager(const TFunction<void(FStringView)>& LogFunc, FStringView Msg)
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
                    LogPackager(LogFunc, FString().sprintf("  + %s -> %s",
                        FileName.c_str(), DstName.string().c_str()).c_str());
                }
                else
                {
                    LogPackager(LogFunc, FString().sprintf("  [warn] failed to copy %s", FileName.c_str()).c_str());
                }
            }

            if (Skipped > 0)
            {
                LogPackager(LogFunc, FString().sprintf("  (skipped %zu wrong-config / editor-only / stale files)", Skipped).c_str());
            }
            return Copied;
        }

        // Recursively copies a directory tree (overwriting existing files). Returns the file count copied.
        size_t CopyDirectoryRecursive(const std::filesystem::path& Src, const std::filesystem::path& Dst)
        {
            std::error_code Ec;
            if (!std::filesystem::exists(Src, Ec))
            {
                return 0;
            }
            size_t Count = 0;
            std::filesystem::create_directories(Dst, Ec);
            for (const auto& Entry : std::filesystem::recursive_directory_iterator(Src, Ec))
            {
                const std::filesystem::path Rel    = std::filesystem::relative(Entry.path(), Src, Ec);
                const std::filesystem::path Target = Dst / Rel;
                if (Entry.is_directory())
                {
                    std::filesystem::create_directories(Target, Ec);
                }
                else if (Entry.is_regular_file())
                {
                    std::filesystem::create_directories(Target.parent_path(), Ec);
                    std::filesystem::copy_file(Entry.path(), Target, std::filesystem::copy_options::overwrite_existing, Ec);
                    if (!Ec)
                    {
                        ++Count;
                    }
                }
            }
            return Count;
        }

        // Stages the C# scripting payload a packaged (monolithic) game needs at runtime, mirroring the
        // exe-relative layout DotNetHost::Initialize + DotNet::LoadCookedScripts probe:
        //   - DotNet/Managed/{LuminaSharp.dll, .runtimeconfig.json, Roslyn + deps}  (managed bootstrap)
        //   - External/DotNet/runtime/<rid>/...                                     (bundled CoreCLR + hostfxr)
        //   - DotNet/Scripts/<Unit>.dll + scripts.manifest.json                     (prebuilt user/plugin scripts)
        void CopyDotNetPayload(const std::filesystem::path& EngineInstallDir,
                               const std::filesystem::path& BinariesDir,
                               const std::filesystem::path& DestDir,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            std::error_code Ec;

            // 1. Managed bootstrap assembly + its dependency closure (Roslyn, runtimeconfig, deps.json).
            const std::filesystem::path ManagedSrc = BinariesDir / "DotNet" / "Managed";
            if (std::filesystem::exists(ManagedSrc, Ec))
            {
                const size_t N = CopyDirectoryRecursive(ManagedSrc, DestDir / "DotNet" / "Managed");
                LogPackager(LogFunc, FString().sprintf("DotNet: staged managed bootstrap (%zu file(s)).", N).c_str());
            }
            else
            {
                LogPackager(LogFunc, FString().sprintf("DotNet: [warn] managed bootstrap not found at %s; C# disabled in package.", ManagedSrc.string().c_str()).c_str());
            }

            // 2. Bundled .NET runtime (whole tree so whatever <rid> the host resolves is present).
            const std::filesystem::path RuntimeSrc = EngineInstallDir / "External" / "DotNet" / "runtime";
            if (std::filesystem::exists(RuntimeSrc, Ec))
            {
                LogPackager(LogFunc, "DotNet: copying bundled .NET runtime (this can take a moment)...");
                const size_t N = CopyDirectoryRecursive(RuntimeSrc, DestDir / "External" / "DotNet" / "runtime");
                LogPackager(LogFunc, FString().sprintf("DotNet: staged .NET runtime (%zu file(s)).", N).c_str());
            }
            else
            {
                LogPackager(LogFunc, FString().sprintf("DotNet: [warn] bundled runtime not found at %s; C# disabled in package.", RuntimeSrc.string().c_str()).c_str());
            }

            // 3. Prebuilt script assemblies + the manifest the cooked loader reads.
            TVector<DotNet::FPackagedScriptUnit> Units;
            DotNet::GatherScriptUnitsForPackaging(Units);

            const std::filesystem::path ScriptsDst = DestDir / "DotNet" / "Scripts";
            FString Manifest = "{\n  \"Units\": [\n";
            size_t Staged = 0;
            for (const DotNet::FPackagedScriptUnit& Unit : Units)
            {
                const std::filesystem::path DllSrc(Unit.DllSourcePath.c_str());
                if (Unit.DllSourcePath.empty() || !std::filesystem::exists(DllSrc, Ec))
                {
                    continue; // unit had no .cs / failed to emit -> nothing to ship
                }
                const FString DllName = Unit.Name + ".dll";
                if (!CopyFileTo(DllSrc, ScriptsDst / DllName.c_str()))
                {
                    LogPackager(LogFunc, FString().sprintf("DotNet: [warn] failed to stage script DLL %s", DllName.c_str()).c_str());
                    continue;
                }

                if (Staged > 0)
                {
                    Manifest += ",\n";
                }
                Manifest += FString().sprintf("    { \"Name\": \"%s\", \"Dll\": \"%s\", \"Deps\": [",
                    Unit.Name.c_str(), DllName.c_str());
                for (size_t i = 0; i < Unit.Deps.size(); ++i)
                {
                    Manifest += FString().sprintf("%s\"%s\"", (i == 0 ? "" : ", "), Unit.Deps[i].c_str());
                }
                Manifest += "] }";
                ++Staged;
            }
            Manifest += "\n  ]\n}\n";

            if (Staged > 0)
            {
                std::filesystem::create_directories(ScriptsDst, Ec);
                std::ofstream Out(ScriptsDst / "scripts.manifest.json", std::ios::binary | std::ios::trunc);
                if (Out)
                {
                    Out.write(Manifest.c_str(), (std::streamsize)Manifest.size());
                }
                LogPackager(LogFunc, FString().sprintf("DotNet: staged %zu prebuilt script assembly(ies) + manifest.", Staged).c_str());
            }
            else
            {
                LogPackager(LogFunc, "DotNet: no C# script assemblies to stage (project ships no scripts).");
            }
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

        const FString Config = Options.BuildConfiguration.empty() ? FString("Shipping") : Options.BuildConfiguration;
        const FString EngineDir = FString(Paths::GetEngineInstallDirectory().c_str());

        // The project's own target, built as a Game. Naming the project rather than the engine is
        // what makes the game module part of the build: the engine comes along as its dependency,
        // and is reused rather than rebuilt when it is already current.
        const FString ProjectDir = Options.ProjectDirectory;
        const FString BuildTool  = EngineDir + "/LuminaBuild.bat";

        FString Args = FString(ProjectName.data(), ProjectName.size());
        Args += " -TargetType=Game";
        Args += " -Configuration=" + Config;

        if (!ProjectDir.empty())
        {
            Args += " -Project=\"" + ProjectDir + "\"";
        }

        LogPackager(LogFunc, FString().sprintf("Building with LuminaBuildTool: Build %s", Args.c_str()).c_str());

        Args = FString("Build ") + Args;

        const std::wstring BuildToolW(BuildTool.begin(), BuildTool.end());
        const std::wstring ArgsW(Args.begin(), Args.end());
        const std::wstring CwdW(EngineDir.begin(), EngineDir.end());

        const int ExitCode = Platform::RunProcessAndWaitCapture(
            BuildToolW.c_str(), ArgsW.c_str(), CwdW.c_str(),
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
                "Build failed (exit code %d). See log above for the build error. Cooked PAK is still at %.*s.",
                ExitCode, (int)PakPath.size(), PakPath.data());
            return Result;
        }

        const std::filesystem::path BinariesDir =
            std::filesystem::path(Paths::GetEngineInstallDirectory().c_str()) / "Binaries" / "Windows64";
        const std::filesystem::path DestDir(Options.OutputDirectory.c_str());

        LogPackager(LogFunc, FString().sprintf("Copying %s binaries from %s",
            Config.c_str(), BinariesDir.string().c_str()).c_str());

        size_t Copied = CopyRuntimePayload(BinariesDir, DestDir, Config, ProjectName, LogFunc);

        // The project's own modules link into the project tree, not the engine's, so a packaged
        // game needs both. Engine binaries are shared by every project and stay where they are.
        if (!ProjectDir.empty())
        {
            const std::filesystem::path ProjectBinaries =
                std::filesystem::path(ProjectDir.c_str()) / "Binaries" / "Windows64";

            std::error_code Ec;
            if (std::filesystem::exists(ProjectBinaries, Ec))
            {
                LogPackager(LogFunc, FString().sprintf("Copying project binaries from %s",
                    ProjectBinaries.string().c_str()).c_str());

                Copied += CopyRuntimePayload(ProjectBinaries, DestDir, Config, ProjectName, LogFunc);
            }
        }

        if (Copied == 0)
        {
            Result.ErrorMessage = "The build succeeded but no matching binaries were found to copy. Check the build output.";
            return Result;
        }
        LogPackager(LogFunc, FString().sprintf("Copied %zu runtime files.", Copied).c_str());

        // Stage the C# scripting payload (managed bootstrap, bundled .NET runtime, prebuilt script DLLs +
        // manifest) so the cooked game can boot CoreCLR and load its scripts without the editor/dev tree.
        CopyDotNetPayload(
            std::filesystem::path(Paths::GetEngineInstallDirectory().c_str()),
            BinariesDir,
            DestDir,
            LogFunc);

        Result.bSuccess = true;
        return Result;
    }

    size_t FProjectPackager::ExtractLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc)
    {
        return CopyLooseScripts(OutDir, LogFunc);
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

        LogPackager(LogFunc, FString().sprintf("Output directory: %s", OutDir.c_str()).c_str());

        const FString PakPath = OutDir + "/" + ProjectName + ".pak";
        LogPackager(LogFunc, FString().sprintf("Cooking PAK: %s", PakPath.c_str()).c_str());

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
        LogPackager(LogFunc, FString().sprintf("Cook OK: %zu assets, %zu extras, %zu bytes", Cook.NumAssetsCooked, Cook.NumExtraFiles, Cook.TotalBytes).c_str());

        if (Options.bExtractScriptsAsLooseFiles)
        {
            LogPackager(LogFunc, "Extracting loose /Game files...");
            const size_t Extracted = CopyLooseScripts(OutDir, LogFunc);
            LogPackager(LogFunc, FString().sprintf("Extracted %zu loose script files.", Extracted).c_str());
        }

        if (Options.bBuildExecutable)
        {
            FPackageBuildOptions LocalOpts = Options;
            LocalOpts.OutputDirectory = OutDir;

            // Resolved here, on the main thread, so the build stage never has to reach for GEngine.
            if (LocalOpts.ProjectDirectory.empty())
            {
                LocalOpts.ProjectDirectory =
                    FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size());
            }
            const FPackageBuildResult Sub = BuildAndCopyOnly(LocalOpts, ProjectName, PakPath, LogFunc);
            if (!Sub.bSuccess)
            {
                Result.ErrorMessage = Sub.ErrorMessage;
                return Result;
            }
        }
        else
        {
            LogPackager(LogFunc, "Skipped executable build (cook only).");
        }

        Result.bSuccess = true;
        LogPackager(LogFunc, "Package complete.");
        return Result;
    }
}
