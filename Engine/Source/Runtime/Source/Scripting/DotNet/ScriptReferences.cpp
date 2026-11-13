#include "RuntimePCH.h"

#include "ScriptReferences.h"

#include "Containers/Span.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Scripting/ManagedTypeRegistry.h"

#include <string>

#include "Core/Json/Json.h"

namespace Lumina::DotNet
{
    namespace
    {
        bool IsAbsolutePath(FStringView Path)
        {
            if (Path.Len() >= 2 && Path[1] == ':')
            {
                return true;
            }
            return !Path.IsEmpty() && (Path[0] == '/' || Path[0] == '\\');
        }

        bool WriteTextFile(FStringView Path, const FString& Text)
        {
            return Filesystem::WriteFile(Path,
                TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len()));
        }

        // Not Paths::Normalize, which also collapses doubled slashes and so breaks a UNC prefix.
        FString Slashed(FStringView Path)
        {
            FString Result(Path.GetData(), Path.Len());
            Algo::Replace(Result, '\\', '/');
            return Result;
        }

        // A restore is skipped while this matches, so it must cover every input that changes the outcome.
        FString PackageFingerprint(const FScriptReferenceSet& Declared)
        {
            FString Text;
            for (const FScriptPackageRef& Package : Declared.Packages)
            {
                Text += Package.Name;
                Text += "/";
                Text += Package.Version;
                Text += ";";
            }
            return Text;
        }

        FString DeclarationFingerprint(const FScriptReferenceSet& Declared)
        {
            FString Text = PackageFingerprint(Declared);
            for (const FString& Assembly : Declared.Assemblies)
            {
                Text += Assembly;
                Text += ";";
            }
            return Text;
        }

        // Spares every reload, packaging pass, and project generation a re-read of each unit's assets file.
        const Scripting::FScriptReferenceResolution* FindCachedResolution(FStringView UnitName, const FString& Fingerprint)
        {
            for (const Scripting::FScriptReferenceResolution& Entry : Scripting::FManagedTypeRegistry::Get().GetScriptReferenceResolutions())
            {
                if (Entry.UnitName == UnitName && Entry.Fingerprint == Fingerprint)
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        void StoreCachedResolution(FStringView UnitName, const FString& Fingerprint, const TVector<FString>& Assemblies)
        {
            TVector<Scripting::FScriptReferenceResolution>& Entries = Scripting::FManagedTypeRegistry::Get().GetScriptReferenceResolutions();
            for (Scripting::FScriptReferenceResolution& Entry : Entries)
            {
                if (Entry.UnitName == UnitName)
                {
                    Entry.Fingerprint = Fingerprint;
                    Entry.Assemblies  = Assemblies;
                    return;
                }
            }
            Entries.Emplace(FString(UnitName), Fingerprint, Assemblies);
        }

        FString BuildRestoreProject(const FScriptReferenceSet& Declared)
        {
            FString Xml;
            Xml += "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
            Xml += "  <PropertyGroup>\n";
            Xml += "    <TargetFramework>net10.0</TargetFramework>\n";
            Xml += "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n";
            Xml += "    <NoWarn>NU1701</NoWarn>\n";
            Xml += "  </PropertyGroup>\n";
            Xml += "  <ItemGroup>\n";
            for (const FScriptPackageRef& Package : Declared.Packages)
            {
                Xml += "    <PackageReference Include=\"" + Package.Name + "\" Version=\"" + Package.Version + "\" />\n";
            }
            Xml += "  </ItemGroup>\n";
            Xml += "</Project>\n";
            return Xml;
        }

        // Prefers the implementation assemblies, since a reference-only assembly cannot be loaded at run time.
        void CollectLibraryAssets(const FJsonValue& Library, TVector<FString>& Out)
        {
            const char* Sections[] = { "runtime", "compile" };
            for (const char* Section : Sections)
            {
                for (const FJsonMember& Entry : Library[Section].Members())
                {
                    const FStringView Relative = Entry.Key.View();
                    if (Relative.EndsWith("_._"))
                    {
                        continue;
                    }
                    Out.Emplace(Relative);
                }
                if (!Out.IsEmpty())
                {
                    return;
                }
            }
        }

        bool ReadAssetPaths(FStringView AssetsPath, TVector<FString>& Out)
        {
            FString Text;
            if (!Filesystem::ReadFile(Text, AssetsPath))
            {
                return false;
            }

            FJsonValue J;
            FJsonParseError Error;
            if (!FJsonValue::Parse(Text.View(), J, &Error))
            {
                LOG_ERROR("C#: failed to parse '{}': {}", AssetsPath, Error.Message);
                return false;
            }

            TVector<FString> Folders;
            for (const FJsonMember& Entry : J["packageFolders"].Members())
            {
                Folders.Emplace(Entry.Key.View());
            }
            if (Folders.IsEmpty())
            {
                LOG_ERROR("C#: '{}' names no package folder; cannot resolve packages.", AssetsPath);
                return false;
            }

            const FJsonValue& Libraries = J["libraries"];
            const FJsonValue& Targets   = J["targets"];
            if (!Targets.IsObject())
            {
                return false;
            }

            for (const FJsonMember& Target : Targets.Members())
            {
                for (const FJsonMember& Entry : Target.Value.Members())
                {
                    if (!Entry.Value.IsObject() || Entry.Value["type"].AsString() != "package")
                    {
                        continue;
                    }

                    // The library's own folder name, which differs from the target key by case.
                    const FStringView Relative = Libraries[Entry.Key.View()]["path"].AsString(Entry.Key.View());

                    TVector<FString> Assets;
                    CollectLibraryAssets(Entry.Value, Assets);
                    for (const FString& Asset : Assets)
                    {
                        for (const FString& Folder : Folders)
                        {
                            const FString Candidate(Paths::Combine(Slashed(Folder), Relative, Asset));
                            if (Filesystem::Exists(Candidate))
                            {
                                Out.Add(Candidate);
                                break;
                            }
                        }
                    }
                }
            }
            return true;
        }

        bool RunRestore(FStringView UnitName, FStringView ProjectPath)
        {
            FString Params = "restore \"";
            Params.Append(ProjectPath.GetData(), ProjectPath.Len());
            Params += "\"";

            FString Output;
            const int ExitCode = Platform::RunProcessAndWaitCapture(
                TEXT("dotnet"), UTF8_TO_TCHAR(*Params), nullptr,
                [&Output](FStringView Line)
                {
                    Output.Append(Line.GetData(), Line.Len());
                    Output += "\n";
                });

            if (ExitCode != 0)
            {
                LOG_ERROR("C#: package restore for unit '{}' failed (exit {}).\n{}", UnitName, ExitCode, Output);
                return false;
            }
            return true;
        }
    }

    bool ParseScriptReferences(FStringView DescriptorPath, FStringView RootDir, FScriptReferenceSet& Out)
    {
        FString Text;
        if (!Filesystem::ReadFile(Text, DescriptorPath))
        {
            return false;
        }

        FJsonValue J;
        if (!FJsonValue::Parse(Text.View(), J) || !J.IsObject())
        {
            return false; // the descriptor's own loader reports malformed JSON
        }

        const FJsonValue& Section = J["ScriptReferences"];
        if (!Section.IsObject())
        {
            return false;
        }

        for (const FJsonValue& Entry : Section["Packages"].AsArray())
        {
            if (!Entry.IsObject())
            {
                continue;
            }
            FScriptPackageRef Package;
            Package.Name    = Entry.GetString("Name");
            Package.Version = Entry.GetString("Version");
            if (Package.Name.IsEmpty() || Package.Version.IsEmpty())
            {
                LOG_WARN("C#: '{}' declares a package with no Name or Version; skipping it.", DescriptorPath);
                continue;
            }
            Out.Packages.Add(Move(Package));
        }

        for (const FJsonValue& Entry : Section["Assemblies"].AsArray())
        {
            if (!Entry.IsString())
            {
                continue;
            }
            const FStringView View = Entry.AsString();
            Out.Assemblies.Add(IsAbsolutePath(View) ? Slashed(View) : Slashed(Paths::Combine(RootDir, View)));
        }

        return !Out.IsEmpty();
    }

    bool ResolveScriptReferences(FStringView UnitName, FStringView RestoreDir,
        const FScriptReferenceSet& Declared, TVector<FString>& OutAssemblyPaths)
    {
        const FString CacheKey = DeclarationFingerprint(Declared);
        if (const Scripting::FScriptReferenceResolution* Cached = FindCachedResolution(UnitName, CacheKey))
        {
            for (const FString& Assembly : Cached->Assemblies)
            {
                OutAssemblyPaths.Add(Assembly);
            }
            return true;
        }

        TVector<FString> Resolved;
        bool bMissingAssembly = false;
        for (const FString& Assembly : Declared.Assemblies)
        {
            if (Filesystem::Exists(Assembly))
            {
                Resolved.Add(Assembly);
            }
            else
            {
                LOG_ERROR("C#: unit '{}' references '{}', which does not exist.", UnitName, Assembly);
                bMissingAssembly = true;
            }
        }

        if (!Declared.Packages.IsEmpty())
        {
            if (RestoreDir.IsEmpty())
            {
                LOG_ERROR("C#: unit '{}' declares packages but has no intermediate directory to restore into.", UnitName);
                return false;
            }

            Filesystem::MakeDirectoryTree(RestoreDir);

            const FString ProjectPath(Paths::Combine(RestoreDir, FString(UnitName) + ".Packages.csproj"));
            const FString AssetsPath(Paths::Combine(RestoreDir, "obj/project.assets.json"));
            const FString StampPath(Paths::Combine(RestoreDir, "packages.stamp"));
            const FString Stamp        = PackageFingerprint(Declared);

            FString Previous;
            const bool bStampMatches = Filesystem::ReadFile(Previous, StampPath) && Previous == Stamp;
            if (!bStampMatches || !Filesystem::Exists(AssetsPath))
            {
                WriteTextFile(ProjectPath, BuildRestoreProject(Declared));

                LOG_DISPLAY("C#: restoring {} package(s) for unit '{}'...", Declared.Packages.Num(), UnitName);
                if (!RunRestore(UnitName, ProjectPath))
                {
                    return false;
                }
                WriteTextFile(StampPath, Stamp);
            }

            const int32 Before = Resolved.Num();
            if (!ReadAssetPaths(AssetsPath, Resolved))
            {
                LOG_ERROR("C#: unit '{}' restored but its package assets could not be read.", UnitName);
                return false;
            }
            LOG_DISPLAY("C#: unit '{}' resolved {} package assembly(ies).", UnitName, Resolved.Num() - Before);
        }

        if (!bMissingAssembly)
        {
            StoreCachedResolution(UnitName, CacheKey, Resolved);
        }
        for (const FString& Assembly : Resolved)
        {
            OutAssemblyPaths.Add(Assembly);
        }
        return true;
    }
}
