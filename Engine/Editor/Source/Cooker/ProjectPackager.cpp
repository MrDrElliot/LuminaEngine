#include "ProjectPackager.h"

#include "Platform/Filesystem/PlatformFilesystem.h"

#include "AssetCooker.h"
#include "Core/Engine/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/DotNet/DotNetHost.h"

#include <fstream>
#include "Containers/StringFormat.h"

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

        FStringView StemOf(FStringView Name)
        {
            const size_t Dot = Name.find_last_of('.');
            return Dot == FStringView::npos ? Name : Name.substr(0, Dot);
        }

        bool CopyFileTo(FStringView Src, FStringView Dst)
        {
            Filesystem::MakeParentDirectoryTree(Dst);
            return Filesystem::Copy(Src, Dst, true);
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

            // Preserves the top-level dir name so loose files re-resolve under the same alias at runtime.
            struct FLooseRoot { const char* Alias; const char* Top; };
            const FLooseRoot Roots[] = { { "/Game", "Game" } };

            for (const FLooseRoot& Root : Roots)
            {
                const FString DstRoot = Join(OutDir, Root.Top);
                Filesystem::MakeDirectoryTree(DstRoot);

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

                    // The C# host compiles sources directly, so generated projects and build output never ship.
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

                    const FString Dst = Join(DstRoot, Relative);
                    if (!Filesystem::WriteFile(Dst, TSpan<const uint8>(Bytes.data(), Bytes.size())))
                    {
                        return;
                    }

                    ++Count;
                    if (LogFunc)
                    {
                        LogFunc(Format("  + {}/{}",
                            Root.Top, Relative).c_str());
                    }
                });
            }
            return Count;
        }

        // Stem ends with "-<OtherConfig>"; used to skip wrong-config DLLs from a previous Editor build.
        bool HasOtherConfigSuffix(FStringView Stem, const FString& MyConfig)
        {
            const char* Configs[] = { "Debug", "Development", "Shipping" };
            for (const char* Cfg : Configs)
            {
                if (MyConfig == Cfg)
                {
                    continue;
                }

                FString Suffix = "-";
                Suffix.append(Cfg);

                if (Stem.ends_with(FStringView(Suffix.c_str(), Suffix.size())))
                {
                    return true;
                }
            }
            return false;
        }

        // True if SourceDir has a "<Stem>-<Config>.dll" sibling; identifies stale pre-targetsuffix dupes.
        bool HasSuffixedSibling(FStringView SourceDir, FStringView Stem)
        {
            for (const char* Cfg : { "Debug", "Development", "Shipping" })
            {
                FString Name(Stem.data(), Stem.size());
                Name.push_back('-');
                Name.append(Cfg);
                Name.append(".dll");

                if (Filesystem::Exists(Join(SourceDir, Name)))
                {
                    return true;
                }
            }
            return false;
        }

        // Editor-only / tooling DLLs that aren't needed at runtime; hard-coded since no programmatic check exists.
        bool IsEditorOnlyDll(FStringView FileName)
        {
            // libclang.dll is for the Reflector tool's Clang frontend (compile-time only).
            return FileName == FStringView("libclang.dll");
        }

        size_t CopyRuntimePayload(FStringView SourceDir,
                                  FStringView DestDir,
                                  const FString& ConfigSuffix,
                                  FStringView ProjectName,
                                  const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Copied = 0;
            size_t Skipped = 0;

            FString MyExeName = "Lumina-";
            MyExeName.append(ConfigSuffix);
            MyExeName.append(".exe");

            Filesystem::IterateDirectory(SourceDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                if (Entry.IsDirectory())
                {
                    return;
                }

                const FStringView FileName = Entry.Name;
                const FStringView Ext      = Entry.GetExtension();
                const FStringView Stem     = StemOf(FileName);

                const bool bExe = Ext == FStringView(".exe");
                const bool bDll = Ext == FStringView(".dll");

                // Skip tools / wrong-config exes.
                if (bExe && FileName != FStringView(MyExeName.c_str(), MyExeName.size()))
                {
                    return;
                }

                if (!bDll && !bExe)
                {
                    return;
                }

                if (HasOtherConfigSuffix(Stem, ConfigSuffix))
                {
                    ++Skipped;
                    return;
                }

                // Skip stale Editor-*.dll left over from a prior Editor build.
                if (Stem.starts_with("Editor-"))
                {
                    ++Skipped;
                    return;
                }

                if (IsEditorOnlyDll(FileName))
                {
                    ++Skipped;
                    return;
                }

                // Stale unsuffixed dupe of a now-suffixed DLL (pre-targetsuffix builds).
                if (bDll
                    && Stem.find("-Debug") == FStringView::npos
                    && Stem.find("-Development") == FStringView::npos
                    && Stem.find("-Shipping") == FStringView::npos
                    && HasSuffixedSibling(SourceDir, Stem))
                {
                    ++Skipped;
                    return;
                }

                // Rename launcher to <ProjectName>.exe; safe because it never reads its own filename.
                FString DstName(FileName.data(), FileName.size());
                if (bExe && !ProjectName.empty())
                {
                    DstName.assign(ProjectName.data(), ProjectName.size());
                    DstName.append(".exe");
                }

                if (CopyFileTo(Entry.FullPath, Join(DestDir, DstName)))
                {
                    ++Copied;
                    LogPackager(LogFunc, Format("  + {} -> {}",
                        FileName, DstName.c_str()).c_str());
                }
                else
                {
                    LogPackager(LogFunc, Format("  [warn] failed to copy {}",
                        FileName).c_str());
                }
            });

            if (Skipped > 0)
            {
                LogPackager(LogFunc, Format("  (skipped {} wrong-config / editor-only / stale files)", Skipped).c_str());
            }
            return Copied;
        }

        // Recursively copies a directory tree (overwriting existing files). Returns the file count copied.
        size_t CopyDirectoryRecursive(FStringView Src, FStringView Dst)
        {
            uint32 Count = 0;
            Filesystem::CopyTree(Src, Dst, &Count);
            return Count;
        }

        // Mirrors the exe-relative layout DotNetHost::Initialize and DotNet::LoadCookedScripts probe.
        void CopyDotNetPayload(FStringView EngineInstallDir,
                               FStringView BinariesDir,
                               FStringView DestDir,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            // 1. Managed bootstrap assembly + its dependency closure (Roslyn, runtimeconfig, deps.json).
            const FString ManagedSrc = Join(BinariesDir, "DotNet/Managed");
            if (Filesystem::Exists(ManagedSrc))
            {
                const size_t N = CopyDirectoryRecursive(ManagedSrc, Join(DestDir, "DotNet/Managed"));
                LogPackager(LogFunc, Format("DotNet: staged managed bootstrap ({} file(s)).", N).c_str());
            }
            else
            {
                LogPackager(LogFunc, Format("DotNet: [warn] managed bootstrap not found at {}; C# disabled in package.", ManagedSrc.c_str()).c_str());
            }

            // 2. Bundled .NET runtime (whole tree so whatever <rid> the host resolves is present).
            const FString RuntimeSrc = Join(EngineInstallDir, "External/DotNet/runtime");
            if (Filesystem::Exists(RuntimeSrc))
            {
                LogPackager(LogFunc, "DotNet: copying bundled .NET runtime (this can take a moment)...");
                const size_t N = CopyDirectoryRecursive(RuntimeSrc, Join(DestDir, "External/DotNet/runtime"));
                LogPackager(LogFunc, Format("DotNet: staged .NET runtime ({} file(s)).", N).c_str());
            }
            else
            {
                LogPackager(LogFunc, Format("DotNet: [warn] bundled runtime not found at {}; C# disabled in package.", RuntimeSrc.c_str()).c_str());
            }

            // 3. Prebuilt script assemblies + the manifest the cooked loader reads.
            TVector<DotNet::FPackagedScriptUnit> Units;
            DotNet::GatherScriptUnitsForPackaging(Units);

            const FString ScriptsDst = Join(DestDir, "DotNet/Scripts");
            FString Manifest = "{\n  \"Units\": [\n";
            size_t Staged = 0;
            for (const DotNet::FPackagedScriptUnit& Unit : Units)
            {
                if (Unit.DllSourcePath.empty() || !Filesystem::Exists(Unit.DllSourcePath))
                {
                    continue; // unit had no .cs / failed to emit -> nothing to ship
                }
                const FString DllName = Unit.Name + ".dll";
                if (!CopyFileTo(Unit.DllSourcePath, Join(ScriptsDst, DllName)))
                {
                    LogPackager(LogFunc, Format("DotNet: [warn] failed to stage script DLL {}", DllName.c_str()).c_str());
                    continue;
                }

                if (Staged > 0)
                {
                    Manifest += ",\n";
                }
                AppendFormat(Manifest, "    {{ \"Name\": \"{}\", \"Dll\": \"{}\", \"Deps\": [",
                    Unit.Name.c_str(), DllName.c_str());
                for (size_t i = 0; i < Unit.Deps.size(); ++i)
                {
                    AppendFormat(Manifest, "{}\"{}\"", (i == 0 ? "" : ", "), Unit.Deps[i].c_str());
                }
                Manifest += "] }";
                ++Staged;
            }
            Manifest += "\n  ]\n}\n";

            if (Staged > 0)
            {
                const TSpan<const uint8> ManifestBytes(reinterpret_cast<const uint8*>(Manifest.data()), Manifest.size());
                Filesystem::WriteFile(Join(ScriptsDst, "scripts.manifest.json"), ManifestBytes);
                LogPackager(LogFunc, Format("DotNet: staged {} prebuilt script assembly(ies) + manifest.", Staged).c_str());
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

        // Naming the project is what puts the game module in the build, with the engine as a dependency.
        const FString ProjectDir = Options.ProjectDirectory;
        const FString BuildTool  = EngineDir + "/LuminaBuild.bat";

        FString Args = FString(ProjectName.data(), ProjectName.size());
        Args += " -TargetType=Game";
        Args += " -Configuration=" + Config;

        if (!ProjectDir.empty())
        {
            Args += " -Project=\"" + ProjectDir + "\"";
        }

        LogPackager(LogFunc, Format("Building with LuminaBuildTool: Build {}", Args.c_str()).c_str());

        Args = FString("Build ") + Args;

        const int ExitCode = Platform::RunProcessAndWaitCapture(
            UTF8_TO_TCHAR(BuildTool.c_str()), UTF8_TO_TCHAR(Args.c_str()), UTF8_TO_TCHAR(EngineDir.c_str()),
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
            Result.ErrorMessage = Format("Build failed (exit code {}). See log above for the build error. Cooked PAK is still at {}.",
                ExitCode, PakPath);
            return Result;
        }

        const FString BinariesDir = Join(Paths::GetEngineInstallDirectory(), "Binaries/Windows64");
        const FString& DestDir    = Options.OutputDirectory;

        LogPackager(LogFunc, Format("Copying {} binaries from {}",
            Config.c_str(), BinariesDir.c_str()).c_str());

        size_t Copied = CopyRuntimePayload(BinariesDir, DestDir, Config, ProjectName, LogFunc);

        // Project modules link into the project tree, while engine binaries stay shared where they are.
        if (!ProjectDir.empty())
        {
            const FString ProjectBinaries = Join(ProjectDir, "Binaries/Windows64");

            if (Filesystem::Exists(ProjectBinaries))
            {
                LogPackager(LogFunc, Format("Copying project binaries from {}",
                    ProjectBinaries.c_str()).c_str());

                Copied += CopyRuntimePayload(ProjectBinaries, DestDir, Config, ProjectName, LogFunc);
            }
        }

        if (Copied == 0)
        {
            Result.ErrorMessage = "The build succeeded but no matching binaries were found to copy. Check the build output.";
            return Result;
        }
        LogPackager(LogFunc, Format("Copied {} runtime files.", Copied).c_str());

        // Lets the cooked game boot CoreCLR and load its scripts without the editor or dev tree.
        CopyDotNetPayload(Paths::GetEngineInstallDirectory(), BinariesDir, DestDir, LogFunc);

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

        if (!Filesystem::MakeDirectoryTree(OutDir))
        {
            Result.ErrorMessage = FString("Failed to create output directory: ") + OutDir;
            return Result;
        }
        Result.OutputDirectory = OutDir;

        LogPackager(LogFunc, Format("Output directory: {}", OutDir.c_str()).c_str());

        const FString PakPath = OutDir + "/" + ProjectName + ".pak";
        LogPackager(LogFunc, Format("Cooking PAK: {}", PakPath.c_str()).c_str());

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
        LogPackager(LogFunc, Format("Cook OK: {} assets, {} extras, {} bytes", Cook.NumAssetsCooked, Cook.NumExtraFiles, Cook.TotalBytes).c_str());

        if (Options.bExtractScriptsAsLooseFiles)
        {
            LogPackager(LogFunc, "Extracting loose /Game files...");
            const size_t Extracted = CopyLooseScripts(OutDir, LogFunc);
            LogPackager(LogFunc, Format("Extracted {} loose script files.", Extracted).c_str());
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
