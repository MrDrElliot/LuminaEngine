#include "RuntimePCH.h"
#include "ScriptUnits.h"
#include "DotNetHost.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include <cstdio>
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Engine/Engine.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Core/Json/Json.h"

namespace Lumina::DotNet
{
    // File name without directory or extension, which for a managed assembly is also its simple name.
    FString StemOf(FStringView Path)
    {
        const int32 Slash = Path.FindLastOf("/\\");
        const int32 Start = Slash == Constants::kIndexNone ? 0 : Slash + 1;
        const int32 Dot   = Path.FindLast('.');
        const size_t End   = (Dot == Constants::kIndexNone || Dot < Start) ? Path.Len() : Dot;
        return FString(Path.GetData() + Start, IntCastChecked<int32>(End - Start));
    }

    FString NativePath(FStringView P)
    {
        FString Result(P.GetData(), P.Len());
    #if defined(_WIN32)
        Algo::Replace(Result, '/', '\\');
    #endif
        return Result;
    }

    // Scripts are source rather than VFS content, so a code-only plugin still gets its scripts compiled.
    template<typename TVisit>
    void ForEachScriptSource(const FString& DiskDir, TVisit&& Visit)
    {
        if (DiskDir.IsEmpty() || !Filesystem::IsDirectory(DiskDir))
        {
            return;
        }

        Filesystem::IterateDirectoryRecursive(DiskDir, [&Visit](const Filesystem::FDirectoryEntry& Entry)
        {
            if (Entry.IsDirectory() || Entry.GetExtension() != FStringView(".cs"))
            {
                return;
            }

            const FStringView P = Entry.FullPath;
            if (P.Find("/obj/") != Constants::kIndexNone || P.Find("/bin/") != Constants::kIndexNone)
            {
                return;
            }
            Visit(P);
        });
    }

    void GatherSourcesUnder(const FString& DiskDir, TVector<FGatheredSource>& Out)
    {
        ForEachScriptSource(DiskDir, [&Out](FStringView P)
        {
            FGatheredSource Src;
            if (!Filesystem::ReadFile(Src.Text, P))
            {
                return;
            }

            Src.Path = FStringView(P.GetData(), P.Len());
            Out.Add(Move(Src));
        });
    }
    

    // Descriptor-declared references, resolved (restoring packages when the declaration changed).
    void ApplyScriptReferences(FScriptUnit& Unit, FStringView DescriptorPath, FStringView RootDir)
    {
        if (DescriptorPath.IsEmpty() || !Filesystem::Exists(DescriptorPath))
        {
            return;
        }
        if (!ParseScriptReferences(DescriptorPath, RootDir, Unit.Declared))
        {
            return;
        }

        const FString RestoreDir = Unit.IntermediateDir.IsEmpty()
            ? FString()
            : Unit.IntermediateDir + "/Packages";

        if (!ResolveScriptReferences(Unit.Name, RestoreDir, Unit.Declared, Unit.References))
        {
            LOG_ERROR("C#: unit '{}' will compile without its declared references.", Unit.Name);
        }
    }

    // The engine convention is one descriptor named for the project beside its content.
    FString FindProjectDescriptor(FStringView ProjectDir)
    {
        FString Found;
        Filesystem::IterateDirectory(ProjectDir, [&Found](const Filesystem::FDirectoryEntry& Entry)
        {
            if (Entry.IsFile() && Entry.GetExtension() == ".lproject")
            {
                Found = FStringView(Entry.FullPath.GetData(), Entry.FullPath.Len());
                return Filesystem::EVisit::Stop;
            }
            return Filesystem::EVisit::Continue;
        });
        return Found;
    }

    // Each unit compiles into its OWN DLL, and a disabled plugin produces no unit at all.
    TVector<FScriptUnit> BuildScriptUnits()
    {
        TVector<FScriptUnit> Units;
        TVector<FString>     PluginNames;

        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (Plugin == nullptr || !Plugin->IsEnabled())
            {
                continue;
            }

            FScriptUnit Unit;
            Unit.Name = FString(Plugin->GetName());
            const FString PluginDir(Plugin->GetDirectory().GetData(), Plugin->GetDirectory().Len());
            Unit.DiskDir         = PluginDir + "/Scripts";
            Unit.BinaryDir       = PluginDir + "/Binaries/DotNet";
            Unit.IntermediateDir = PluginDir + "/Intermediates/DotNet/" + Unit.Name;
            Unit.AssemblyPath    = Unit.BinaryDir + "/" + Unit.Name + ".dll";
            for (const FPluginDependency& Dep : Plugin->GetDescriptor().Dependencies)
            {
                Unit.Deps.Add(Dep.Name);
            }

            ApplyScriptReferences(Unit, Plugin->GetDescriptorPath(), PluginDir);

            PluginNames.Add(Unit.Name);
            Units.Add(Move(Unit));
        }

        {
            FScriptUnit Game;
            Game.Name = "Game";
            if (GEngine != nullptr)
            {
                const FFixedString ScriptsDir = GEngine->GetProjectScriptsDirectory();
                Game.DiskDir = FStringView(ScriptsDir.GetData(), ScriptsDir.Len());

                const FStringView ProjectPath = GEngine->GetProjectPath();
                if (!ProjectPath.IsEmpty())
                {
                    const FString Root(ProjectPath.GetData(), ProjectPath.Len());
                    Game.BinaryDir       = Root + "/Binaries/DotNet";
                    Game.IntermediateDir = Root + "/Intermediates/DotNet/Game";
                    Game.AssemblyPath    = Game.BinaryDir + "/Game.dll";

                    ApplyScriptReferences(Game, FindProjectDescriptor(Root), Root);
                }
            }
            Game.Deps = PluginNames;
            Units.Add(Move(Game));
        }

        {
            FScriptUnit Engine;
            Engine.Name = "Engine";
            Engine.DiskDir = Paths::GetEngineResourceDirectory() + "/Scripts";
            // Engine example scripts live next to the engine binaries (a sibling of DotNet/Managed, not in it).
            const FString ExeDir = Paths::Parent(Platform::GetCurrentProcessPath());
            Engine.BinaryDir       = NativePath(Paths::Combine(ExeDir, "DotNet"));
            Engine.IntermediateDir = NativePath(Paths::Combine(ExeDir, "DotNet/obj/Engine"));
            Engine.AssemblyPath    = Engine.BinaryDir + "/Engine.dll";
            Units.Add(Move(Engine));
        }

        return Units;
    }

    // Returns empty when no manifest is present, as in a dev run or a project with no C#.
    TVector<FScriptUnit> BuildCookedScriptUnits()
    {
        TVector<FScriptUnit> Units;

        // A package keeps its script assemblies with LuminaSharp in the data folder's Managed directory.
        const FString ScriptsDir(Paths::Combine(Paths::GetGameDataDirectory(), "Managed"));
        const FString ManifestPath(Paths::Combine(ScriptsDir, "scripts.manifest.json"));

        FString Text;
        if (!Filesystem::ReadFile(Text, ManifestPath))
        {
            return Units;
        }

        FJsonValue J;
        FJsonParseError Error;
        if (!FJsonValue::Parse(Text.View(), J, &Error))
        {
            LOG_ERROR("C#: failed to parse cooked script manifest '{}': {}", ManifestPath, Error.Message);
            return Units;
        }

        const FJsonValue& Arr = J.Contains("Units") ? J["Units"] : J;
        if (!Arr.IsArray())
        {
            return Units;
        }

        for (const FJsonValue& Entry : Arr.AsArray())
        {
            if (!Entry.IsObject())
            {
                continue;
            }
            const FString Name = Entry.GetString("Name");
            if (Name.IsEmpty())
            {
                continue;
            }

            FScriptUnit Unit;
            Unit.Name = Name;
            const FString Dll = Entry.GetString("Dll", (Name + ".dll").View());
            Unit.AssemblyPath = Paths::Combine(ScriptsDir, Dll);

            for (const FJsonValue& Dep : Entry["Deps"].AsArray())
            {
                if (Dep.IsString())
                {
                    Unit.Deps.Emplace(Dep.AsString());
                }
            }

            // Staged beside the unit DLLs, so the cooked game never needs the NuGet cache.
            for (const FJsonValue& Reference : Entry["References"].AsArray())
            {
                if (Reference.IsString())
                {
                    Unit.References.Emplace(Paths::Combine(ScriptsDir, Reference.AsString()));
                }
            }
            Units.Add(Move(Unit));
        }
        return Units;
    }

    // So frequent reloads do not churn the files and the IDE does not reload projects each time.
    void WriteTextIfChanged(FStringView Path, const FString& Content)
    {
        FString Existing;
        if (Filesystem::ReadFile(Existing, Path) && Existing == Content)
        {
            return;
        }

        const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(Content.GetData()), Content.Len());
        if (Filesystem::WriteFile(Path, Bytes))
        {
            LOG_DISPLAY("Generated C# script project: {}", Path);
        }
    }

    FString CompiledMarkerPath(const FString& DllPath)
    {
        return DllPath + ".compiled";
    }

    // Stable across regenerations, so the IDE does not treat each regen as a brand-new project.
    FString MakeStableGuid(const FString& Seed)
    {
        auto Fnv = [](const char* S, uint64 Basis) -> uint64
        {
            uint64 H = Basis;
            for (; *S != '\0'; ++S)
            {
                H = (H ^ static_cast<uint64>(static_cast<uint8>(*S))) * 1099511628211ULL;
            }
            return H;
        };

        const uint64 A = Fnv(*Seed, 14695981039346656037ULL);
        const uint64 B = Fnv(*Seed, 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL);
        uint8 Bytes[16];
        for (int Index = 0; Index < 8; ++Index)
        {
            Bytes[Index]     = static_cast<uint8>(A >> (Index * 8));
            Bytes[8 + Index] = static_cast<uint8>(B >> (Index * 8));
        }

        char Buf[40];
        std::snprintf(Buf, sizeof(Buf),
            "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], Bytes[5], Bytes[6], Bytes[7],
            Bytes[8], Bytes[9], Bytes[10], Bytes[11], Bytes[12], Bytes[13], Bytes[14], Bytes[15]);
        return FString(Buf);
    }

    // The absolute path of the generated IDE project for a unit ("<DiskDir>/<Name>.Scripts.csproj").
    FString UnitProjectPath(const FScriptUnit& Unit)
    {
        return NativePath(Paths::Combine(Unit.DiskDir, Unit.Name + ".Scripts.csproj"));
    }

    // Matches the runtime cross-assembly references exactly, so IntelliSense sees the same graph.
    FString BuildCsprojXml(const FScriptUnit& Unit, const TVector<FScriptUnit>& AllUnits, const FString& LuminaSharpDll)
    {
        FString Xml;
        Xml += "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
        Xml += "  <!-- GENERATED for IDE IntelliSense only (run \"dotnet.genprojects\" to refresh).\n";
        Xml += "       Scripts are compiled at runtime by the engine; this project is never the runtime\n";
        Xml += "       path and is overwritten on reload. -->\n";
        Xml += "  <PropertyGroup>\n";
        Xml += "    <TargetFramework>net10.0</TargetFramework>\n";
        Xml += "    <Nullable>enable</Nullable>\n";
        Xml += "    <ImplicitUsings>disable</ImplicitUsings>\n";
        Xml += "    <EnableDefaultItems>true</EnableDefaultItems>\n";
        // Generated reflected-type bindings read properties through raw pointers.
        Xml += "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
        Xml += "    <AssemblyName>" + Unit.Name + "</AssemblyName>\n";
        // An IDE build produces the same artifact and nothing lands in the engine library's output.
        if (!Unit.BinaryDir.IsEmpty())
        {
            Xml += "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
            Xml += "    <OutputPath>" + Unit.BinaryDir + "/</OutputPath>\n";
        }
        if (!Unit.IntermediateDir.IsEmpty())
        {
            Xml += "    <IntermediateOutputPath>" + Unit.IntermediateDir + "/</IntermediateOutputPath>\n";
        }
        Xml += "  </PropertyGroup>\n";
        Xml += "  <ItemGroup>\n";
        Xml += "    <Reference Include=\"LuminaSharp\">\n";
        Xml += "      <HintPath>" + LuminaSharpDll + "</HintPath>\n";
        Xml += "    </Reference>\n";
        // The generator the runtime compile loads; without it partial NativeCall bodies are missing.
        Xml += "    <Analyzer Include=\""
            + NativePath(Paths::Combine(Paths::Parent(LuminaSharpDll), "LuminaSharp.Generators.dll"))
            + "\" />\n";
        // Matches the imports ScriptCompiler adds to every runtime compile, so LoadObject<T> and bare specifiers resolve the same in the IDE.
        Xml += "    <Using Include=\"LuminaSharp.ObjectCore\" Static=\"true\" />\n";
        Xml += "    <Using Include=\"LuminaSharp.ESpecifier\" Static=\"true\" />\n";
        Xml += "  </ItemGroup>\n";

        // Named as packages rather than as their restored DLLs, so the IDE restores them the usual way.
        if (!Unit.Declared.Packages.IsEmpty() || !Unit.Declared.Assemblies.IsEmpty())
        {
            Xml += "  <ItemGroup>\n";
            for (const FScriptPackageRef& Package : Unit.Declared.Packages)
            {
                Xml += "    <PackageReference Include=\"" + Package.Name
                    + "\" Version=\"" + Package.Version + "\" />\n";
            }
            for (const FString& Assembly : Unit.Declared.Assemblies)
            {
                const FString Stem = StemOf(Assembly);
                Xml += "    <Reference Include=\"" + Stem + "\">\n";
                Xml += "      <HintPath>" + NativePath(Assembly) + "</HintPath>\n";
                Xml += "    </Reference>\n";
            }
            Xml += "  </ItemGroup>\n";
        }

        FString Refs;
        for (const FString& DepName : Unit.Deps)
        {
            const FScriptUnit* Dep = nullptr;
            for (const FScriptUnit& Candidate : AllUnits)
            {
                if (Candidate.Name == DepName)
                {
                    Dep = &Candidate;
                    break;
                }
            }
            if (Dep == nullptr || Dep->DiskDir.IsEmpty() || !Filesystem::Exists(Dep->DiskDir))
            {
                continue; // the dependency ships no scripts on disk; nothing to reference
            }
            Refs += "    <ProjectReference Include=\"" + UnitProjectPath(*Dep) + "\" />\n";
        }
        if (!Refs.IsEmpty())
        {
            Xml += "  <ItemGroup>\n" + Refs + "  </ItemGroup>\n";
        }

        Xml += "</Project>\n";
        return Xml;
    }

    // Placed at the project root, and skipped when no project is loaded.
    void WriteScriptSolution(const TVector<FScriptUnit>& Units)
    {
        if (GEngine == nullptr || GEngine->GetProjectPath().IsEmpty())
        {
            return;
        }

        struct FSlnEntry { FString Name; FString Path; FString Guid; };
        TVector<FSlnEntry> Entries;
        for (const FScriptUnit& Unit : Units)
        {
            if (Unit.DiskDir.IsEmpty())
            {
                continue;
            }
            if (!Filesystem::Exists(Unit.DiskDir))
            {
                continue;
            }
            Entries.Emplace(Unit.Name + ".Scripts", UnitProjectPath(Unit), MakeStableGuid(Unit.Name));
        }
        if (Entries.IsEmpty())
        {
            return;
        }

        const char* CsProjTypeGuid = "{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}";
        FString Sln;
        Sln += "\xEF\xBB\xBF";
        Sln += "Microsoft Visual Studio Solution File, Format Version 12.00\n";
        Sln += "# Visual Studio Version 17\n";
        for (const FSlnEntry& Entry : Entries)
        {
            Sln += "Project(\"" + FString(CsProjTypeGuid) + "\") = \"" + Entry.Name + "\", \"" + Entry.Path + "\", \"" + Entry.Guid + "\"\n";
            Sln += "EndProject\n";
        }
        Sln += "Global\n";
        Sln += "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
        Sln += "\t\tDebug|Any CPU = Debug|Any CPU\n";
        Sln += "\t\tRelease|Any CPU = Release|Any CPU\n";
        Sln += "\tEndGlobalSection\n";
        Sln += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
        for (const FSlnEntry& Entry : Entries)
        {
            Sln += "\t\t" + Entry.Guid + ".Debug|Any CPU.ActiveCfg = Debug|Any CPU\n";
            Sln += "\t\t" + Entry.Guid + ".Release|Any CPU.ActiveCfg = Release|Any CPU\n";
        }
        Sln += "\tEndGlobalSection\n";
        Sln += "EndGlobal\n";

        FString ProjectName(GEngine->GetProjectName().GetData(), GEngine->GetProjectName().Len());
        if (ProjectName.IsEmpty())
        {
            ProjectName = "Game";
        }
        const FString SlnPath = NativePath(Paths::Combine(GEngine->GetProjectPath(), ProjectName + ".GameScripts.sln"));
        WriteTextIfChanged(SlnPath, Sln);
    }

    FString FindScriptSourceFile(FStringView TypeName)
    {
        if (TypeName.IsEmpty())
        {
            return FString();
        }

        // The stem, since a script is declared in a file named after it by every convention the templates set.
        const int32 Dot = TypeName.FindLastOf('.');
        const FStringView Short = Dot == Constants::kIndexNone ? TypeName : TypeName.Mid(Dot + 1);

        FString Wanted(Short.GetData(), Short.Len());
        Wanted += ".cs";

        FString Found;
        for (const FScriptUnit& Unit : BuildScriptUnits())
        {
            if (Unit.DiskDir.IsEmpty() || !Filesystem::Exists(Unit.DiskDir))
            {
                continue;
            }

            Filesystem::IterateDirectoryRecursive(Unit.DiskDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                if (Entry.IsDirectory() || !Found.IsEmpty())
                {
                    return true;
                }
                if (Entry.Name.Len() == Wanted.Len()
                    && EqualsIgnoreCase(Entry.Name, FStringView(Wanted)))
                {
                    Found = FStringView(Entry.FullPath.GetData(), Entry.FullPath.Len());
                    return false;
                }
                return true;
            });

            if (!Found.IsEmpty())
            {
                break;
            }
        }

        return Found;
    }

    void GatherScriptSourceFolders(TVector<FScriptSourceFolder>& Out)
    {
        Out.Reset();

        TVector<FScriptUnit> Units;
        for (FScriptUnit& Unit : BuildScriptUnits())
        {
            if (!Unit.DiskDir.IsEmpty() && Filesystem::Exists(Unit.DiskDir))
            {
                Units.Add(Move(Unit));
            }
        }

        auto ToForwardSlashes = [](FString Path)
        {
            for (char& Character : Path)
            {
                Character = Character == '\\' ? '/' : Character;
            }
            return Path;
        };

        // Collected per unit first, since the unit name only earns a folder level when more than one unit has scripts.
        TVector<TVector<FScriptSourceFolder>> PerUnit(Units.Num());
        uint32 UnitsWithScripts = 0;
        for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
        {
            const FString Root = ToForwardSlashes(Units[UnitIndex].DiskDir);
            Filesystem::IterateDirectoryRecursive(Units[UnitIndex].DiskDir, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                const FStringView Name(Entry.Name);
                if (Entry.IsDirectory() || Name.Len() <= 3 || !EqualsIgnoreCase(Name.Mid(Name.Len() - 3), FStringView(".cs")))
                {
                    return true;
                }

                FString Relative = ToForwardSlashes(FString(Entry.FullPath.GetData(), Entry.FullPath.Len()));
                if (Relative.Len() > Root.Len() && Relative.StartsWith(Root))
                {
                    Relative.RemoveAt(0, Root.Len());
                }
                while (!Relative.IsEmpty() && Relative[0] == '/')
                {
                    Relative.RemoveAt(0, 1);
                }

                const int32 LastSlash = Relative.FindLastOf('/');
                FScriptSourceFolder& Found = PerUnit[UnitIndex].EmplaceGetRef();
                Found.Stem   = FString(Name.Mid(0, Name.Len() - 3));
                Found.Folder = LastSlash == Constants::kIndexNone ? FString() : Relative.Mid(0, LastSlash);
                Found.Path   = FString(Entry.FullPath.GetData(), Entry.FullPath.Len());
                return true;
            });
            UnitsWithScripts += PerUnit[UnitIndex].IsEmpty() ? 0u : 1u;
        }

        for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
        {
            for (FScriptSourceFolder& Found : PerUnit[UnitIndex])
            {
                if (UnitsWithScripts > 1)
                {
                    Found.Folder = Found.Folder.IsEmpty() ? Units[UnitIndex].Name : Units[UnitIndex].Name + "/" + Found.Folder;
                }
                Out.Add(Move(Found));
            }
        }
    }

    void GatherScriptSourceFiles(TVector<FString>& OutPaths)
    {
        OutPaths.Reset();

        // The same roots BuildScriptUnits compiles, minus the reference resolution that can restore packages.
        TVector<FString> Roots;
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (Plugin != nullptr && Plugin->IsEnabled())
            {
                Roots.Add(FString(Plugin->GetDirectory().GetData(), Plugin->GetDirectory().Len()) + "/Scripts");
            }
        }
        if (GEngine != nullptr)
        {
            const FFixedString ScriptsDir = GEngine->GetProjectScriptsDirectory();
            Roots.Emplace(ScriptsDir.GetData(), ScriptsDir.Len());
        }
        Roots.Add(Paths::GetEngineResourceDirectory() + "/Scripts");

        for (const FString& Root : Roots)
        {
            ForEachScriptSource(Root, [&OutPaths](FStringView P) { OutPaths.Emplace(P.GetData(), P.Len()); });
        }
    }

    void GenerateScriptProjects()
    {
        if (!IsInitialized())
        {
            return;
        }

        const FString ExePath = NativePath(Platform::GetCurrentProcessPath());
        const FString Dll = NativePath(Paths::Combine(Paths::Parent(ExePath), "DotNet/Managed/LuminaSharp.dll"));

        // The same unit graph the runtime compiles, so the IDE view matches what actually builds.
        const TVector<FScriptUnit> Units = BuildScriptUnits();
        for (const FScriptUnit& Unit : Units)
        {
            if (Unit.DiskDir.IsEmpty())
            {
                continue;
            }
            if (!Filesystem::Exists(Unit.DiskDir))
            {
                continue;
            }
            WriteTextIfChanged(UnitProjectPath(Unit), BuildCsprojXml(Unit, Units, Dll));
        }

        WriteScriptSolution(Units);
    }
}
