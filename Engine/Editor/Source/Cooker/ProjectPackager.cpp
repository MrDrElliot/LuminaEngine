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
#include "Containers/HashTable.h"

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

        FString FileNameOf(FStringView Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            const size_t Start = Slash == FStringView::npos ? 0 : Slash + 1;
            return FString(Path.data() + Start, Path.size() - Start);
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
                    LogPackager(LogFunc, Format("  + {}/{}", Root.Top, Relative).c_str());
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

        // A game binary ends in exactly "-<Config>", while every other target type names itself first.
        bool HasForeignTargetElement(FStringView Stem, const FString& MyConfig)
        {
            FString ConfigTail = "-";
            ConfigTail.append(MyConfig);

            if (!Stem.ends_with(FStringView(ConfigTail.c_str(), ConfigTail.size())))
            {
                return false;
            }

            const FStringView Base = Stem.substr(0, Stem.size() - ConfigTail.size());

            for (const char* Type : { "Editor", "Program" })
            {
                FString TypeTail = "-";
                TypeTail.append(Type);

                if (Base.ends_with(FStringView(TypeTail.c_str(), TypeTail.size())))
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

        constexpr const char* BuildRulesSuffix = ".Build.cs";

        // Every module the engine can build, so a game module another project left in the shared Binaries is never shipped.
        THashSet<FString> CollectEngineModuleNames(FStringView EngineDir)
        {
            THashSet<FString> Names;
            for (const char* Root : { "Engine/Source", "Engine/Plugins", "Engine/Editor" })
            {
                Filesystem::IterateDirectoryRecursive(Join(EngineDir, Root), [&Names](const Filesystem::FDirectoryEntry& Entry)
                {
                    const FStringView FileName = Entry.Name;
                    if (!Entry.IsDirectory() && FileName.ends_with(BuildRulesSuffix))
                    {
                        Names.insert(FString(FileName.data(), FileName.size() - strlen(BuildRulesSuffix)));
                    }
                });
            }
            return Names;
        }

        // The module a "<Module>-<Config>" binary belongs to, or empty for a binary without the suffix.
        FStringView ModuleOfSuffixedBinary(FStringView Stem)
        {
            for (const char* Cfg : { "-Debug", "-Development", "-Shipping" })
            {
                if (Stem.ends_with(Cfg))
                {
                    return Stem.substr(0, Stem.size() - strlen(Cfg));
                }
            }
            return {};
        }

        // Binaries an earlier package wrote that this one did not, which the game would otherwise carry forever.
        // The reflector's Clang and the editor-only Nsight and BugSplat SDKs never load in a game, and Shipping compiles Aftermath out.
        bool IsUnusedByGameDll(FStringView FileName, FStringView ConfigSuffix)
        {
            if (FileName == FStringView("libclang.dll") || FileName == FStringView("nvperf_grfx_host.dll") || FileName.starts_with("BugSplat"))
            {
                return true;
            }
            return ConfigSuffix == FStringView("Shipping") && FileName.starts_with("GFSDK_Aftermath");
        }

        void RemoveStaleBinaries(FStringView DestDir, const THashSet<FString>& WrittenStems, FStringView ConfigSuffix,
                                 const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<FString> Stale;
            Filesystem::IterateDirectory(DestDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                const FStringView Ext = Entry.GetExtension();
                if (Entry.IsDirectory() || (Ext != FStringView(".dll") && Ext != FStringView(".pdb")))
                {
                    return;
                }
                const FStringView Stem = StemOf(Entry.Name);
                const bool bUnwritten = !WrittenStems.contains(FString(Stem.data(), Stem.size()));
                if (bUnwritten && (!ModuleOfSuffixedBinary(Stem).empty() || IsUnusedByGameDll(Entry.Name, ConfigSuffix)))
                {
                    Stale.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
                }
            });

            for (const FString& Path : Stale)
            {
                if (Filesystem::RemoveFile(FStringView(Path.c_str(), Path.size())))
                {
                    LogPackager(LogFunc, Format("  - removed stale {}", FileNameOf(Path).c_str()).c_str());
                }
            }
        }

        // In a monolithic build the engine, plugin and game modules are inside the executable, and the exe to ship is the project's.
        struct FMonolithicCopy
        {
            bool                     bEnabled = false;
            bool                     bCopyExecutable = true;
            const THashSet<FString>* LinkedModules = nullptr;

            // Where third-party DLLs go, since the monolithic exe imports none of them at startup.
            FString                  PluginsDirectory;
        };

        size_t CopyRuntimePayload(FStringView SourceDir,
                                  FStringView DestDir,
                                  const FString& ConfigSuffix,
                                  FStringView ProjectName,
                                  const THashSet<FString>* AllowedModules,
                                  THashSet<FString>& WrittenStems,
                                  const TFunction<void(FStringView)>& LogFunc,
                                  const FMonolithicCopy& Monolithic = {})
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
                if (bExe && (FileName != FStringView(MyExeName.c_str(), MyExeName.size()) || !Monolithic.bCopyExecutable))
                {
                    return;
                }

                // A stale module DLL here would load beside the executable's own copy of the same code.
                if (bDll && Monolithic.bEnabled)
                {
                    const FStringView Linked = ModuleOfSuffixedBinary(Stem);
                    if (!Linked.empty() && (Linked == ProjectName
                        || (Monolithic.LinkedModules != nullptr && Monolithic.LinkedModules->contains(FString(Linked.data(), Linked.size())))))
                    {
                        ++Skipped;
                        return;
                    }
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

                // An editor or program build of the same module, which exports a different surface.
                if (HasForeignTargetElement(Stem, ConfigSuffix))
                {
                    ++Skipped;
                    return;
                }

                // The editor module under its pre-rename name, which no game build ever produces.
                if (Stem.starts_with("Editor-"))
                {
                    ++Skipped;
                    return;
                }

                if (IsUnusedByGameDll(FileName, FStringView(ConfigSuffix.c_str(), ConfigSuffix.size())))
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

                // Another project's game module, built into the shared engine Binaries and never loaded by this game.
                const FStringView Module = ModuleOfSuffixedBinary(Stem);
                if (AllowedModules != nullptr && bDll && !Module.empty() && Module != ProjectName
                    && !AllowedModules->contains(FString(Module.data(), Module.size())))
                {
                    ++Skipped;
                    LogPackager(LogFunc, Format("  (skipped {}, not an engine module)", FileName).c_str());
                    return;
                }

                // Rename launcher to <ProjectName>.exe; safe because it never reads its own filename.
                FString DstName(FileName.data(), FileName.size());
                if (bExe && !ProjectName.empty())
                {
                    DstName.assign(ProjectName.data(), ProjectName.size());
                    DstName.append(".exe");
                }

                // Only a monolithic exe is free of startup imports, so only there do third-party DLLs leave the exe's folder.
                const bool bToPlugins = bDll && Monolithic.bEnabled && !Monolithic.PluginsDirectory.empty() && ModuleOfSuffixedBinary(Stem).empty();
                const FString DstDir = bToPlugins ? Monolithic.PluginsDirectory : FString(DestDir.data(), DestDir.size());

                if (CopyFileTo(Entry.FullPath, Join(DstDir, DstName)))
                {
                    ++Copied;
                    const FStringView DstStem = StemOf(FStringView(DstName.c_str(), DstName.size()));
                    WrittenStems.insert(FString(DstStem.data(), DstStem.size()));
                    LogPackager(LogFunc, Format("  + {} -> {}{}",
                        FileName, bToPlugins ? "Plugins/" : "", DstName.c_str()).c_str());
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

        // Paks used to sit beside the exe, where the game would still mount them over the data folder's.
        void RemoveOldPaks(FStringView OutDir, const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<FString> Old;
            Filesystem::IterateDirectory(OutDir, [&Old](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Entry.IsDirectory() && (Entry.GetExtension() == FStringView(".pak") || Entry.Name.ends_with(".contents.txt")))
                {
                    Old.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
                }
            });
            for (const FString& Path : Old)
            {
                if (Filesystem::RemoveFile(FStringView(Path.c_str(), Path.size())))
                {
                    LogPackager(LogFunc, Format("  - removed {} from the old layout", FileNameOf(Path).c_str()).c_str());
                }
            }
        }

        // The runtime, managed code and third-party DLLs are restaged into the data folder, so earlier copies would only shadow them.
        void RemoveOldPayload(FStringView OutDir, FStringView DataDir, const TFunction<void(FStringView)>& LogFunc)
        {
            const FStringView OldDirectories[] = { "DotNet", "External" };
            for (const FStringView Directory : OldDirectories)
            {
                const FString Path = Join(OutDir, Directory);
                if (Filesystem::Exists(Path) && Filesystem::RemoveTree(Path))
                {
                    LogPackager(LogFunc, Format("  - removed {}/ from the old layout", FString(Directory.data(), Directory.size()).c_str()).c_str());
                }
            }

            const FStringView StagedDirectories[] = { "Managed", "Runtime", "Plugins" };
            for (const FStringView Directory : StagedDirectories)
            {
                const FString Path = Join(DataDir, Directory);
                if (Filesystem::Exists(Path))
                {
                    Filesystem::RemoveTree(Path);
                }
            }

            TVector<FString> ThirdParty;
            Filesystem::IterateDirectory(OutDir, [&ThirdParty](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Entry.IsDirectory() && Entry.GetExtension() == FStringView(".dll") && ModuleOfSuffixedBinary(StemOf(Entry.Name)).empty())
                {
                    ThirdParty.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
                }
            });
            for (const FString& Path : ThirdParty)
            {
                Filesystem::RemoveFile(FStringView(Path.c_str(), Path.size()));
            }
        }

        // Recursively copies a directory tree (overwriting existing files). Returns the file count copied.
        size_t CopyDirectoryRecursive(FStringView Src, FStringView Dst)
        {
            uint32 Count = 0;
            Filesystem::CopyTree(Src, Dst, &Count);
            return Count;
        }

        // Mirrors the data-folder layout DotNetHost::Initialize and DotNet::LoadCookedScripts probe.
        void CopyDotNetPayload(FStringView EngineInstallDir,
                               FStringView BinariesDir,
                               FStringView DataDir,
                               bool bStageSymbols,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            // 1. The managed bootstrap alone, since scripts arrive prebuilt and the script compiler and its generators never run in a game.
            const FString ManagedSrc = Join(BinariesDir, "DotNet/Managed");
            const FString ManagedDst = Join(DataDir, "Managed");
            const FStringView BootstrapFiles[] = { "LuminaSharp.dll", "LuminaSharp.runtimeconfig.json" };
            size_t BootstrapCopied = 0;
            for (const FStringView File : BootstrapFiles)
            {
                BootstrapCopied += CopyFileTo(Join(ManagedSrc, File), Join(ManagedDst, File)) ? 1 : 0;
            }
            if (bStageSymbols)
            {
                CopyFileTo(Join(ManagedSrc, "LuminaSharp.pdb"), Join(ManagedDst, "LuminaSharp.pdb"));
            }
            if (BootstrapCopied == std::size(BootstrapFiles))
            {
                LogPackager(LogFunc, "DotNet: staged the managed bootstrap.");
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
                const size_t N = CopyDirectoryRecursive(RuntimeSrc, Join(DataDir, "Runtime"));
                LogPackager(LogFunc, Format("DotNet: staged .NET runtime ({} file(s)).", N).c_str());
            }
            else
            {
                LogPackager(LogFunc, Format("DotNet: [warn] bundled runtime not found at {}; C# disabled in package.", RuntimeSrc.c_str()).c_str());
            }

            // 3. Prebuilt script assemblies + the manifest the cooked loader reads, beside the bootstrap that loads them.
            TVector<DotNet::FPackagedScriptUnit> Units;
            DotNet::GatherScriptUnitsForPackaging(Units);

            const FString& ScriptsDst = ManagedDst;
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

                // Without these a script exception in a dev package reports no file or line.
                if (bStageSymbols && Unit.DllSourcePath.ends_with(".dll"))
                {
                    const FString PdbSrc = Unit.DllSourcePath.substr(0, Unit.DllSourcePath.size() - 4) + ".pdb";
                    if (Filesystem::Exists(PdbSrc))
                    {
                        CopyFileTo(PdbSrc, Join(ScriptsDst, Unit.Name + ".pdb"));
                    }
                }

                // Flattened beside the unit DLLs so the cooked game resolves them without the NuGet cache.
                TVector<FString> StagedReferences;
                for (const FString& Reference : Unit.References)
                {
                    const FString FileName = FileNameOf(Reference);
                    if (FileName.empty() || !Filesystem::Exists(Reference))
                    {
                        continue;
                    }
                    if (!CopyFileTo(Reference, Join(ScriptsDst, FileName)))
                    {
                        LogPackager(LogFunc, Format("DotNet: [warn] failed to stage script reference {}", FileName.c_str()).c_str());
                        continue;
                    }
                    StagedReferences.push_back(FileName);
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
                Manifest += "], \"References\": [";
                for (size_t i = 0; i < StagedReferences.size(); ++i)
                {
                    AppendFormat(Manifest, "{}\"{}\"", (i == 0 ? "" : ", "), StagedReferences[i].c_str());
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
                if (!Line.empty())
                {
                    FString Prefixed = FString("  | ");
                    Prefixed.append(Line.data(), Line.size());
                    LogPackager(LogFunc, Prefixed);
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

        const THashSet<FString> EngineModules = CollectEngineModuleNames(Paths::GetEngineInstallDirectory());
        const FString ProjectBinaries = ProjectDir.empty() ? FString() : Join(ProjectDir, "Binaries/Windows64");

        // Shipping links the game into the engine application, which the build leaves in the project's Binaries.
        FMonolithicCopy Monolithic;
        Monolithic.bEnabled = Config == "Shipping" && !ProjectDir.empty();
        Monolithic.LinkedModules = &EngineModules;

        FMonolithicCopy EngineCopy = Monolithic;
        EngineCopy.bCopyExecutable = !Monolithic.bEnabled;

        const FString DataDir = GetDataDirectory(FStringView(DestDir.c_str(), DestDir.size()), ProjectName);
        Monolithic.PluginsDirectory = Join(DataDir, "Plugins");
        EngineCopy.PluginsDirectory = Monolithic.PluginsDirectory;

        // Staged fresh each build, and an older package kept its DLLs, runtime and managed code beside the exe.
        RemoveOldPayload(DestDir, DataDir, LogFunc);

        THashSet<FString> WrittenStems;
        size_t Copied = CopyRuntimePayload(BinariesDir, DestDir, Config, ProjectName, &EngineModules, WrittenStems, LogFunc, EngineCopy);

        // Project modules link into the project tree, while engine binaries stay shared where they are.
        if (!ProjectBinaries.empty() && Filesystem::Exists(ProjectBinaries))
        {
            LogPackager(LogFunc, Format("Copying project binaries from {}",
                ProjectBinaries.c_str()).c_str());

            Copied += CopyRuntimePayload(ProjectBinaries, DestDir, Config, ProjectName, nullptr, WrittenStems, LogFunc, Monolithic);
        }

        if (Copied == 0)
        {
            Result.ErrorMessage = "The build succeeded but no matching binaries were found to copy. Check the build output.";
            return Result;
        }
        LogPackager(LogFunc, Format("Copied {} runtime files.", Copied).c_str());
        RemoveStaleBinaries(DestDir, WrittenStems, FStringView(Config.c_str(), Config.size()), LogFunc);

        // Lets the cooked game boot CoreCLR and load its scripts without the editor or dev tree.
        CopyDotNetPayload(Paths::GetEngineInstallDirectory(), BinariesDir, DataDir, !(Config == "Shipping"), LogFunc);

        Result.bSuccess = true;
        return Result;
    }

    FString FProjectPackager::GetDataDirectory(FStringView OutputDirectory, FStringView ProjectName)
    {
        FString Directory(OutputDirectory.data(), OutputDirectory.size());
        Directory += "/";
        Directory.append(ProjectName.data(), ProjectName.size());
        Directory += "_Data";
        return Directory;
    }

    FString FProjectPackager::GetPakPath(FStringView OutputDirectory, FStringView ProjectName)
    {
        FString Path = GetDataDirectory(OutputDirectory, ProjectName);
        Path += "/";
        Path.append(ProjectName.data(), ProjectName.size());
        Path += ".pak";
        return Path;
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

        const FString PakPath = GetPakPath(FStringView(OutDir.c_str(), OutDir.size()), FStringView(ProjectName.c_str(), ProjectName.size()));
        RemoveOldPaks(OutDir, LogFunc);
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
            const size_t Extracted = CopyLooseScripts(GetDataDirectory(FStringView(OutDir.c_str(), OutDir.size()), FStringView(ProjectName.c_str(), ProjectName.size())), LogFunc);
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
