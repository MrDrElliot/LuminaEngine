#include "ProjectPackager.h"

#include "Platform/Filesystem/PlatformFilesystem.h"

#include "AssetCooker.h"
#include "Core/Engine/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/DotNet/DotNetHost.h"

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
            LOG_INFO("[Packager] {}", *FString(Msg.GetData(), Msg.Len()));
        }

        FString FileNameOf(FStringView Path)
        {
            const int32 Slash = Path.FindLastOf("/\\");
            const size_t Start = Slash == Constants::kIndexNone ? 0 : Slash + 1;
            return FString(Path.GetData() + Start, IntCastChecked<int32>(Path.Len() - Start));
        }

        bool CopyFileTo(FStringView Src, FStringView Dst)
        {
            Filesystem::MakeParentDirectoryTree(Dst);
            return Filesystem::Copy(Src, Dst, true);
        }

        // Mirror every non-.lasset /Game/ file under <OutDir>/Game/ for loose-files mode.
        size_t CopyLooseScripts(const FString& OutDir, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;

            // Preserves the top-level dir name so loose files re-resolve under the same alias at runtime.
            struct FLooseRoot { const char* Alias; const char* Top; };
            const FLooseRoot Roots[] = { { "/Game", "Game" } };

            for (const FLooseRoot& Root : Roots)
            {
                const FFixedString DstRoot = Paths::Combine(OutDir, Root.Top);
                Filesystem::MakeDirectoryTree(DstRoot);

                const FString Alias(Root.Alias);
                const FString Prefix = Alias + "/";

                VFS::RecursiveDirectoryIterator(FStringView(*Alias, Alias.Len()), [&](const VFS::FFileInfo& Info)
                {
                    if (Info.IsDirectory())
                    {
                        return;
                    }

                    FStringView Vp(*Info.VirtualPath, Info.VirtualPath.Len());
                    if (EndsWithIgnoreCase(Vp, ".lasset") || IsDevelopmentOnlyGameFile(Vp, true))
                    {
                        return;
                    }

                    if (Vp.Len() <= Prefix.Len())
                    {
                        return;
                    }
                    FStringView Relative = Vp.Mid(Prefix.Len());

                    TVector<uint8> Bytes;
                    if (!VFS::ReadFile(Bytes, Info.VirtualPath))
                    {
                        return;
                    }

                    const FFixedString Dst = Paths::Combine(DstRoot, Relative);
                    if (!Filesystem::WriteFile(Dst, TSpan<const uint8>(Bytes.GetData(), Bytes.Num())))
                    {
                        return;
                    }

                    ++Count;
                    LogPackager(LogFunc, *Format("  + {}/{}", Root.Top, Relative));
                });
            }
            return Count;
        }

        FStringView WithoutLibraryPrefix(const FPackagePlatformRules& Rules, FStringView FileName)
        {
            const bool bPrefixed = !Rules.LibraryPrefix.IsEmpty() && FileName.StartsWith(Rules.LibraryPrefix);
            return bPrefixed ? FileName.Mid(IntCastChecked<int32>(Rules.LibraryPrefix.Len())) : FileName;
        }

        // The tail every binary of this package ends in, "Development" for a game and "Server-Development" for a server.
        FString BinaryTail(const FString& Config, bool bServer)
        {
            return bServer ? FString("Server-") + Config : Config;
        }

        // A suffixed binary that does not end in this package's tail belongs to another configuration or target.
        bool HasOtherConfigSuffix(FStringView Stem, FStringView MyTail)
        {
            FString Mine = "-";
            Mine.Append(MyTail.GetData(), MyTail.Len());
            if (Stem.EndsWith(FStringView(*Mine, Mine.Len())))
            {
                return false;
            }

            for (const char* Cfg : { "-Debug", "-Development", "-Shipping" })
            {
                if (Stem.EndsWith(Cfg))
                {
                    return true;
                }
            }
            return false;
        }

        // A game binary ends in exactly "-<Config>", while every other target type names itself first.
        bool HasForeignTargetElement(FStringView Stem, FStringView MyConfig)
        {
            FString ConfigTail = "-";
            ConfigTail.Append(MyConfig.GetData(), MyConfig.Len());

            if (!Stem.EndsWith(FStringView(*ConfigTail, ConfigTail.Len())))
            {
                return false;
            }

            const FStringView Base = Stem.Mid(0, Stem.Len() - ConfigTail.Len());

            for (const char* Type : { "Editor", "Program", "Server" })
            {
                FString TypeTail = "-";
                TypeTail.Append(Type);

                if (Base.EndsWith(FStringView(*TypeTail, TypeTail.Len())))
                {
                    return true;
                }
            }

            return false;
        }

        // True if SourceDir has a "<Stem>-<Config>" library sibling; identifies stale pre-targetsuffix dupes.
        bool HasSuffixedSibling(const FPackagePlatformRules& Rules, FStringView SourceDir, FStringView Stem)
        {
            for (const char* Cfg : { "Debug", "Development", "Shipping" })
            {
                FString Suffixed(Stem.GetData(), Stem.Len());
                Suffixed.AppendChar('-');
                Suffixed.Append(Cfg);

                if (Filesystem::Exists(Paths::Combine(SourceDir, Rules.LibraryFileName(Suffixed))))
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
                Filesystem::IterateDirectoryRecursive(Paths::Combine(EngineDir, Root), [&Names](const Filesystem::FDirectoryEntry& Entry)
                {
                    const FStringView FileName = Entry.Name;
                    if (!Entry.IsDirectory() && FileName.EndsWith(BuildRulesSuffix))
                    {
                        Names.Emplace(FileName.GetData(), IntCastChecked<int32>(FileName.Len() - strlen(BuildRulesSuffix)));
                    }
                });
            }
            return Names;
        }

        // The module a "<Module>-<Config>" or "<Module>-Server-<Config>" binary belongs to, or empty for one without the suffix.
        FStringView ModuleOfSuffixedBinary(FStringView Stem)
        {
            for (const char* Cfg : { "-Debug", "-Development", "-Shipping" })
            {
                if (Stem.EndsWith(Cfg))
                {
                    FStringView Module = Stem.Mid(0, IntCastChecked<int32>(Stem.Len() - strlen(Cfg)));
                    if (Module.EndsWith("-Server"))
                    {
                        Module = Module.Mid(0, IntCastChecked<int32>(Module.Len() - strlen("-Server")));
                    }
                    return Module;
                }
            }
            return {};
        }

        // Binaries an earlier package wrote that this one did not, which the game would otherwise carry forever.
        // The reflector's Clang and the editor-only Nsight and BugSplat SDKs never load in a game, and Shipping compiles Aftermath out.
        bool IsUnusedByGameDll(const FPackagePlatformRules& Rules, FStringView FileName, FStringView ConfigSuffix)
        {
            const FStringView Unprefixed = WithoutLibraryPrefix(Rules, FileName);
            if (Rules.IsLibrary(FileName) && (VFS::RemoveExtension(FileName) == FStringView("libclang") || VFS::RemoveExtension(Unprefixed) == FStringView("nvperf_grfx_host")))
            {
                return true;
            }
            if (Unprefixed.StartsWith("BugSplat"))
            {
                return true;
            }
            return ConfigSuffix.EndsWith("Shipping") && Unprefixed.StartsWith("GFSDK_Aftermath");
        }

        void RemoveStaleBinaries(const FPackagePlatformRules& Rules, FStringView DestDir, const THashSet<FString>& WrittenStems, FStringView ConfigSuffix,
                                 const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<FString> Stale;
            Filesystem::IterateDirectory(DestDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                if (Entry.IsDirectory() || (!Rules.IsLibrary(Entry.Name) && Entry.GetExtension() != FStringView(".pdb")))
                {
                    return;
                }
                const FStringView Stem = VFS::RemoveExtension(Entry.Name);
                const bool bUnwritten = !WrittenStems.Contains(FString(Stem.GetData(), Stem.Len()));
                if (bUnwritten && (!ModuleOfSuffixedBinary(Rules.ModuleStem(Entry.Name)).IsEmpty() || IsUnusedByGameDll(Rules, Entry.Name, ConfigSuffix)))
                {
                    Stale.Emplace(Entry.FullPath.GetData(), Entry.FullPath.Len());
                }
            });

            for (const FString& Path : Stale)
            {
                if (Filesystem::RemoveFile(FStringView(*Path, Path.Len())))
                {
                    LogPackager(LogFunc, *Format("  - removed stale {}", *FileNameOf(Path)));
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

        size_t CopyRuntimePayload(const FPackagePlatformRules& Rules,
                                  FStringView SourceDir,
                                  FStringView DestDir,
                                  const FString& ConfigSuffix,
                                  FStringView ProjectName,
                                  FStringView ExecutableName,
                                  const THashSet<FString>* AllowedModules,
                                  THashSet<FString>& WrittenStems,
                                  const TFunction<void(FStringView)>& LogFunc,
                                  const FMonolithicCopy& Monolithic = {})
        {
            size_t Copied = 0;
            size_t Skipped = 0;

            const FString MyExeName = Rules.ExecutableFileName(*(FString("Lumina-") + ConfigSuffix));

            Filesystem::IterateDirectory(SourceDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                if (Entry.IsDirectory())
                {
                    return;
                }

                const FStringView FileName = Entry.Name;
                const FStringView Stem     = Rules.ModuleStem(FileName);

                const bool bExe = Rules.IsExecutable(FileName);
                const bool bDll = Rules.IsLibrary(FileName);

                // Skip tools / wrong-config exes.
                if (bExe && (FileName != FStringView(*MyExeName, MyExeName.Len()) || !Monolithic.bCopyExecutable))
                {
                    return;
                }

                // A stale module DLL here would load beside the executable's own copy of the same code.
                if (bDll && Monolithic.bEnabled)
                {
                    const FStringView Linked = ModuleOfSuffixedBinary(Stem);
                    if (!Linked.IsEmpty() && (Linked == ProjectName
                        || (Monolithic.LinkedModules != nullptr && Monolithic.LinkedModules->Contains(FString(Linked.GetData(), Linked.Len())))))
                    {
                        ++Skipped;
                        return;
                    }
                }

                if (!bDll && !bExe)
                {
                    return;
                }

                if (Rules.IsSkippedBinary(FileName, FStringView(*ConfigSuffix, ConfigSuffix.Len())))
                {
                    ++Skipped;
                    return;
                }

                // Stale unsuffixed dupe of a now-suffixed DLL (pre-targetsuffix builds).
                if (bDll
                    && Stem.Find("-Debug") == Constants::kIndexNone
                    && Stem.Find("-Development") == Constants::kIndexNone
                    && Stem.Find("-Shipping") == Constants::kIndexNone
                    && HasSuffixedSibling(Rules, SourceDir, Stem))
                {
                    ++Skipped;
                    return;
                }

                // Another project's game module, built into the shared engine Binaries and never loaded by this game.
                const FStringView Module = ModuleOfSuffixedBinary(Stem);
                if (AllowedModules != nullptr && bDll && !Module.IsEmpty() && Module != ProjectName
                    && !AllowedModules->Contains(FString(Module.GetData(), Module.Len())))
                {
                    ++Skipped;
                    LogPackager(LogFunc, *Format("  (skipped {}, not an engine module)", FileName));
                    return;
                }

                // Renamed to the package name, which is also how the executable finds its <Name>_Data folder.
                FString DstName(FileName.GetData(), FileName.Len());
                if (bExe && !ExecutableName.IsEmpty())
                {
                    DstName = Rules.ExecutableFileName(ExecutableName);
                }

                // Only a monolithic exe is free of startup imports, so only there do third-party DLLs leave the exe's folder.
                const bool bToPlugins = bDll && Monolithic.bEnabled && !Monolithic.PluginsDirectory.IsEmpty() && ModuleOfSuffixedBinary(Stem).IsEmpty();
                const FString DstDir = bToPlugins ? Monolithic.PluginsDirectory : FString(DestDir.GetData(), DestDir.Len());

                if (CopyFileTo(Entry.FullPath, Paths::Combine(DstDir, DstName)))
                {
                    ++Copied;
                    const FStringView DstStem = VFS::RemoveExtension(FStringView(*DstName, DstName.Len()));
                    WrittenStems.Emplace(DstStem.GetData(), DstStem.Len());
                    LogPackager(LogFunc, *Format("  + {} -> {}{}",
                        FileName, bToPlugins ? "Plugins/" : "", *DstName));
                }
                else
                {
                    LogPackager(LogFunc, *Format("  [warn] failed to copy {}",
                        FileName));
                }
            });

            if (Skipped > 0)
            {
                LogPackager(LogFunc, *Format("  (skipped {} wrong-config / editor-only / stale files)", Skipped));
            }
            return Copied;
        }

        // Paks used to sit beside the exe, where the game would still mount them over the data folder's.
        void RemoveOldPaks(FStringView OutDir, const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<FString> Old;
            Filesystem::IterateDirectory(OutDir, [&Old](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Entry.IsDirectory() && (Entry.GetExtension() == FStringView(".pak") || Entry.Name.EndsWith(".contents.txt")))
                {
                    Old.Emplace(Entry.FullPath.GetData(), Entry.FullPath.Len());
                }
            });
            for (const FString& Path : Old)
            {
                if (Filesystem::RemoveFile(FStringView(*Path, Path.Len())))
                {
                    LogPackager(LogFunc, *Format("  - removed {} from the old layout", *FileNameOf(Path)));
                }
            }
        }

        // The runtime, managed code and third-party DLLs are restaged into the data folder, so earlier copies would only shadow them.
        void RemoveOldPayload(const FPackagePlatformRules& Rules, FStringView OutDir, FStringView DataDir, const TFunction<void(FStringView)>& LogFunc)
        {
            const FStringView OldDirectories[] = { "DotNet", "External" };
            for (const FStringView Directory : OldDirectories)
            {
                const FFixedString Path = Paths::Combine(OutDir, Directory);
                if (Filesystem::Exists(Path) && Filesystem::RemoveTree(Path))
                {
                    LogPackager(LogFunc, *Format("  - removed {}/ from the old layout", *FString(Directory.GetData(), Directory.Len())));
                }
            }

            const FStringView StagedDirectories[] = { "Managed", "Runtime", "Plugins" };
            for (const FStringView Directory : StagedDirectories)
            {
                const FFixedString Path = Paths::Combine(DataDir, Directory);
                if (Filesystem::Exists(Path))
                {
                    Filesystem::RemoveTree(Path);
                }
            }

            TVector<FString> ThirdParty;
            Filesystem::IterateDirectory(OutDir, [&ThirdParty, &Rules](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Entry.IsDirectory() && Rules.IsLibrary(Entry.Name) && ModuleOfSuffixedBinary(Rules.ModuleStem(Entry.Name)).IsEmpty())
                {
                    ThirdParty.Emplace(Entry.FullPath.GetData(), Entry.FullPath.Len());
                }
            });
            for (const FString& Path : ThirdParty)
            {
                Filesystem::RemoveFile(FStringView(*Path, Path.Len()));
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
        void CopyDotNetPayload(const FPackagePlatformRules& Rules,
                               FStringView EngineInstallDir,
                               FStringView BinariesDir,
                               FStringView DataDir,
                               bool bStageSymbols,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            // 1. The managed bootstrap alone, since scripts arrive prebuilt and the script compiler and its generators never run in a game.
            const FFixedString ManagedSrc = Paths::Combine(BinariesDir, "DotNet/Managed");
            const FFixedString ManagedDst = Paths::Combine(DataDir, "Managed");
            const FStringView BootstrapFiles[] = { "LuminaSharp.dll", "LuminaSharp.runtimeconfig.json" };
            size_t BootstrapCopied = 0;
            for (const FStringView File : BootstrapFiles)
            {
                BootstrapCopied += CopyFileTo(Paths::Combine(ManagedSrc, File), Paths::Combine(ManagedDst, File)) ? 1 : 0;
            }
            if (bStageSymbols)
            {
                CopyFileTo(Paths::Combine(ManagedSrc, "LuminaSharp.pdb"), Paths::Combine(ManagedDst, "LuminaSharp.pdb"));
            }
            if (BootstrapCopied == std::size(BootstrapFiles))
            {
                LogPackager(LogFunc, "DotNet: staged the managed bootstrap.");
            }
            else
            {
                LogPackager(LogFunc, *Format("DotNet: [warn] managed bootstrap not found at {}; C# disabled in package.", *ManagedSrc));
            }

            // 2. The bundled .NET runtime for the target platform, which the host resolves as Runtime/<rid>.
            FFixedString RuntimeSrc = Paths::Combine(EngineInstallDir, "External/DotNet/runtime", Rules.DotNetRuntimeRid);
            if (!Filesystem::Exists(RuntimeSrc))
            {
                // A Linux build in WSL brings its runtime back beside the binaries, since External may be another checkout's.
                RuntimeSrc = Paths::Combine(BinariesDir, "DotNet/runtime", Rules.DotNetRuntimeRid);
            }
            if (Filesystem::Exists(RuntimeSrc))
            {
                LogPackager(LogFunc, "DotNet: copying bundled .NET runtime (this can take a moment)...");
                const size_t N = CopyDirectoryRecursive(RuntimeSrc, Paths::Combine(DataDir, "Runtime", Rules.DotNetRuntimeRid));
                LogPackager(LogFunc, *Format("DotNet: staged .NET runtime ({} file(s)).", N));
            }
            else
            {
                LogPackager(LogFunc, *Format("DotNet: [warn] bundled runtime not found at {}; C# disabled in package.", *RuntimeSrc));
            }

            // 3. Prebuilt script assemblies + the manifest the cooked loader reads, beside the bootstrap that loads them.
            TVector<DotNet::FPackagedScriptUnit> Units;
            DotNet::GatherScriptUnitsForPackaging(Units);

            const FFixedString& ScriptsDst = ManagedDst;
            FString Manifest = "{\n  \"Units\": [\n";
            size_t Staged = 0;
            for (const DotNet::FPackagedScriptUnit& Unit : Units)
            {
                if (Unit.DllSourcePath.IsEmpty() || !Filesystem::Exists(Unit.DllSourcePath))
                {
                    continue; // unit had no .cs / failed to emit -> nothing to ship
                }
                const FString DllName = Unit.Name + ".dll";
                if (!CopyFileTo(Unit.DllSourcePath, Paths::Combine(ScriptsDst, DllName)))
                {
                    LogPackager(LogFunc, *Format("DotNet: [warn] failed to stage script DLL {}", *DllName));
                    continue;
                }

                // Without these a script exception in a dev package reports no file or line.
                if (bStageSymbols && Unit.DllSourcePath.EndsWith(".dll"))
                {
                    const FString PdbSrc = Unit.DllSourcePath.Mid(0, Unit.DllSourcePath.Len() - 4) + ".pdb";
                    if (Filesystem::Exists(PdbSrc))
                    {
                        CopyFileTo(PdbSrc, Paths::Combine(ScriptsDst, Unit.Name + ".pdb"));
                    }
                }

                // Flattened beside the unit DLLs so the cooked game resolves them without the NuGet cache.
                TVector<FString> StagedReferences;
                for (const FString& Reference : Unit.References)
                {
                    const FString FileName = FileNameOf(Reference);
                    if (FileName.IsEmpty() || !Filesystem::Exists(Reference))
                    {
                        continue;
                    }
                    if (!CopyFileTo(Reference, Paths::Combine(ScriptsDst, FileName)))
                    {
                        LogPackager(LogFunc, *Format("DotNet: [warn] failed to stage script reference {}", *FileName));
                        continue;
                    }
                    StagedReferences.Add(FileName);
                }

                if (Staged > 0)
                {
                    Manifest += ",\n";
                }
                AppendFormat(Manifest, "    {{ \"Name\": \"{}\", \"Dll\": \"{}\", \"Deps\": [",
                    *Unit.Name, *DllName);
                for (int32 i = 0; i < Unit.Deps.Num(); ++i)
                {
                    AppendFormat(Manifest, "{}\"{}\"", (i == 0 ? "" : ", "), *Unit.Deps[i]);
                }
                Manifest += "], \"References\": [";
                for (int32 i = 0; i < StagedReferences.Num(); ++i)
                {
                    AppendFormat(Manifest, "{}\"{}\"", (i == 0 ? "" : ", "), *StagedReferences[i]);
                }
                Manifest += "] }";
                ++Staged;
            }
            Manifest += "\n  ]\n}\n";

            if (Staged > 0)
            {
                const TSpan<const uint8> ManifestBytes(reinterpret_cast<const uint8*>(Manifest.GetData()), Manifest.Len());
                Filesystem::WriteFile(Paths::Combine(ScriptsDst, "scripts.manifest.json"), ManifestBytes);
                LogPackager(LogFunc, *Format("DotNet: staged {} prebuilt script assembly(ies) + manifest.", Staged));
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
        const TFunction<void(FStringView)>& LogFunc,
        const FThreadStop* Stop)
    {
        FPackageBuildResult Result;
        Result.OutputDirectory = Options.OutputDirectory;
        Result.PakPath = FStringView(PakPath.GetData(), PakPath.Len());

        const FString Config = Options.BuildConfiguration.IsEmpty() ? FString("Shipping") : Options.BuildConfiguration;
        const FString EngineDir = FString(*Paths::GetEngineInstallDirectory());

        // Naming the project is what puts the game module in the build, with the engine as a dependency.
        const FString ProjectDir = Options.ProjectDirectory;
        const FString BuildTool  = EngineDir + "/LuminaBuild.bat";

        const FString Tail = BinaryTail(Config, Options.bServer);
        const FString PackageName = GetPackageName(ProjectName, Options.bServer);
        const FPackagePlatformRules Rules = FPackagePlatformRules::For(Options.Platform);

        FString Args = FString(ProjectName.GetData(), ProjectName.Len());
        Args += Options.bServer ? " -TargetType=Server" : " -TargetType=Game";
        Args += " -Configuration=" + Config;
        Args += " -Platform=";
        Args.Append(Rules.Name.GetData(), Rules.Name.Len());

        if (!ProjectDir.IsEmpty())
        {
            Args += " -Project=\"" + ProjectDir + "\"";
        }

        LogPackager(LogFunc, *Format("Building with LuminaBuildTool: Build {}", *Args));

        Args = FString("Build ") + Args;

        const int ExitCode = Platform::RunProcessAndWaitCapture(
            UTF8_TO_TCHAR(*BuildTool), UTF8_TO_TCHAR(*Args), UTF8_TO_TCHAR(*EngineDir),
            [&LogFunc](FStringView Line)
            {
                if (!Line.IsEmpty())
                {
                    FString Prefixed = FString("  | ");
                    Prefixed.Append(Line.GetData(), Line.Len());
                    LogPackager(LogFunc, Prefixed);
                }
            },
            0, Stop);

        if (ExitCode != 0)
        {
            Result.ErrorMessage = Format("Build failed (exit code {}). See log above for the build error. Cooked PAK is still at {}.",
                ExitCode, PakPath);
            return Result;
        }

        const FFixedString BinariesDir = Rules.BinariesDirectory(Paths::GetEngineInstallDirectory());
        const FString& DestDir    = Options.OutputDirectory;

        LogPackager(LogFunc, *Format("Copying {} binaries from {}",
            *Config, *BinariesDir));

        const THashSet<FString> EngineModules = CollectEngineModuleNames(Paths::GetEngineInstallDirectory());
        const FFixedString ProjectBinaries = ProjectDir.IsEmpty() ? FFixedString() : Rules.BinariesDirectory(FStringView(*ProjectDir, ProjectDir.Len()));

        // Shipping links the game into the engine application, which the build leaves in the project's Binaries.
        FMonolithicCopy Monolithic;
        Monolithic.bEnabled = Config == "Shipping" && !ProjectDir.IsEmpty();
        Monolithic.LinkedModules = &EngineModules;

        FMonolithicCopy EngineCopy = Monolithic;
        EngineCopy.bCopyExecutable = !Monolithic.bEnabled;

        const FString DataDir = GetDataDirectory(FStringView(*DestDir, DestDir.Len()), FStringView(*PackageName, PackageName.Len()));
        if (Rules.bThirdPartyToDataPlugins)
        {
            Monolithic.PluginsDirectory = FString(*Paths::Combine(DataDir, "Plugins"));
            EngineCopy.PluginsDirectory = Monolithic.PluginsDirectory;
        }

        // Staged fresh each build, and an older package kept its DLLs, runtime and managed code beside the exe.
        RemoveOldPayload(Rules, DestDir, DataDir, LogFunc);

        THashSet<FString> WrittenStems;
        const FStringView ExecutableName(*PackageName, PackageName.Len());
        size_t Copied = CopyRuntimePayload(Rules, BinariesDir, DestDir, Tail, ProjectName, ExecutableName, &EngineModules, WrittenStems, LogFunc, EngineCopy);

        // Project modules link into the project tree, while engine binaries stay shared where they are.
        if (!ProjectBinaries.IsEmpty() && Filesystem::Exists(ProjectBinaries))
        {
            LogPackager(LogFunc, *Format("Copying project binaries from {}",
                *ProjectBinaries));

            Copied += CopyRuntimePayload(Rules, ProjectBinaries, DestDir, Tail, ProjectName, ExecutableName, nullptr, WrittenStems, LogFunc, Monolithic);
        }

        if (Copied == 0)
        {
            Result.ErrorMessage = "The build succeeded but no matching binaries were found to copy. Check the build output.";
            return Result;
        }
        LogPackager(LogFunc, *Format("Copied {} runtime files.", Copied));
        RemoveStaleBinaries(Rules, DestDir, WrittenStems, FStringView(*Tail, Tail.Len()), LogFunc);

        // Lets the cooked game boot CoreCLR and load its scripts without the editor or dev tree.
        CopyDotNetPayload(Rules, Paths::GetEngineInstallDirectory(), BinariesDir, DataDir, !(Config == "Shipping"), LogFunc);

        Result.bSuccess = true;
        return Result;
    }

    FPackagePlatformRules FPackagePlatformRules::For(EPackagePlatform Platform)
    {
        if (Platform == EPackagePlatform::Linux64)
        {
            return { "Linux64", "", "lib", ".so", "linux-x64", false };
        }
        return { "Windows64", ".exe", "", ".dll", "win-x64", true };
    }

    FFixedString FPackagePlatformRules::BinariesDirectory(FStringView Root) const
    {
        return Paths::Combine(Root, "Binaries", Name);
    }

    FString FPackagePlatformRules::ExecutableFileName(FStringView Stem) const
    {
        FString FileName(Stem.GetData(), Stem.Len());
        FileName.Append(ExecutableExtension.GetData(), ExecutableExtension.Len());
        return FileName;
    }

    FString FPackagePlatformRules::LibraryFileName(FStringView Stem) const
    {
        FString FileName(LibraryPrefix.GetData(), LibraryPrefix.Len());
        FileName.Append(Stem.GetData(), Stem.Len());
        FileName.Append(LibraryExtension.GetData(), LibraryExtension.Len());
        return FileName;
    }

    bool FPackagePlatformRules::IsExecutable(FStringView FileName) const
    {
        return EqualsIgnoreCase(VFS::Extension(FileName), ExecutableExtension);
    }

    bool FPackagePlatformRules::IsLibrary(FStringView FileName) const
    {
        if (EqualsIgnoreCase(VFS::Extension(FileName), LibraryExtension))
        {
            return true;
        }

        // A versioned soname such as libslang-rt.so.0.2026.3.1 keeps its version after the extension.
        FString Versioned(LibraryExtension.GetData(), LibraryExtension.Len());
        Versioned.AppendChar('.');
        return FileName.Find(*Versioned) != Constants::kIndexNone;
    }

    FStringView FPackagePlatformRules::ModuleStem(FStringView FileName) const
    {
        return VFS::RemoveExtension(WithoutLibraryPrefix(*this, FileName));
    }

    bool FPackagePlatformRules::IsSkippedBinary(FStringView FileName, FStringView BinaryTail) const
    {
        const FStringView Stem = ModuleStem(FileName);

        // An editor or program build of the same module exports a different surface, and no game build names a module Editor.
        return HasOtherConfigSuffix(Stem, BinaryTail) || HasForeignTargetElement(Stem, BinaryTail) || Stem.StartsWith("Editor-")
            || IsUnusedByGameDll(*this, FileName, BinaryTail);
    }

    FString FProjectPackager::GetPackageName(FStringView ProjectName, bool bServer)
    {
        FString Name(ProjectName.GetData(), ProjectName.Len());
        if (bServer)
        {
            Name += "Server";
        }
        return Name;
    }

    FString FProjectPackager::GetDataDirectory(FStringView OutputDirectory, FStringView ProjectName)
    {
        FString Directory(OutputDirectory.GetData(), OutputDirectory.Len());
        Directory += "/";
        Directory.Append(ProjectName.GetData(), ProjectName.Len());
        Directory += "_Data";
        return Directory;
    }

    FString FProjectPackager::GetPakPath(FStringView OutputDirectory, FStringView ProjectName)
    {
        FString Path = GetDataDirectory(OutputDirectory, ProjectName);
        Path += "/";
        Path.Append(ProjectName.GetData(), ProjectName.Len());
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

        if (Global::GEngine == nullptr || Global::GEngine->GetProjectName().IsEmpty())
        {
            Result.ErrorMessage = "No project loaded.";
            return Result;
        }

        const FString ProjectName(Global::GEngine->GetProjectName().GetData(), Global::GEngine->GetProjectName().Len());
        const FString PackageName = GetPackageName(FStringView(*ProjectName, ProjectName.Len()), Options.bServer);

        FString OutDir = Options.OutputDirectory;
        if (OutDir.IsEmpty())
        {
            OutDir = FString(Global::GEngine->GetProjectPath().GetData(), Global::GEngine->GetProjectPath().Len()) + "/Build/" + PackageName;
        }

        if (!Filesystem::MakeDirectoryTree(OutDir))
        {
            Result.ErrorMessage = FString("Failed to create output directory: ") + OutDir;
            return Result;
        }
        Result.OutputDirectory = OutDir;

        LogPackager(LogFunc, *Format("Output directory: {}", *OutDir));

        const FString PakPath = GetPakPath(FStringView(*OutDir, OutDir.Len()), FStringView(*PackageName, PackageName.Len()));
        RemoveOldPaks(OutDir, LogFunc);
        LogPackager(LogFunc, *Format("Cooking PAK: {}", *PakPath));

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
        LogPackager(LogFunc, *Format("Cook OK: {} assets, {} extras, {} bytes", Cook.NumAssetsCooked, Cook.NumExtraFiles, Cook.TotalBytes));

        if (Options.bExtractScriptsAsLooseFiles)
        {
            LogPackager(LogFunc, "Extracting loose /Game files...");
            const size_t Extracted = CopyLooseScripts(GetDataDirectory(FStringView(*OutDir, OutDir.Len()), FStringView(*PackageName, PackageName.Len())), LogFunc);
            LogPackager(LogFunc, *Format("Extracted {} loose script files.", Extracted));
        }

        if (Options.bBuildExecutable)
        {
            FPackageBuildOptions LocalOpts = Options;
            LocalOpts.OutputDirectory = OutDir;

            // Resolved here, on the main thread, so the build stage never has to reach for GEngine.
            if (LocalOpts.ProjectDirectory.IsEmpty())
            {
                LocalOpts.ProjectDirectory =
                    FString(Global::GEngine->GetProjectPath().GetData(), Global::GEngine->GetProjectPath().Len());
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
