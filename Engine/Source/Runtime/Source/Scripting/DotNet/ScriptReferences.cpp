#include "RuntimePCH.h"

#include "ScriptReferences.h"

#include "Containers/Span.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"

#include <string>

#include "nlohmann/json.hpp"

namespace Lumina::DotNet
{
    namespace
    {
        FString JoinPath(FStringView A, FStringView B)
        {
            FString Result(A.data(), A.size());
            if (!Result.empty() && Result.back() != '/' && Result.back() != '\\')
            {
                Result += "/";
            }
            Result.append(B.data(), B.size());
            return Result;
        }

        bool IsAbsolutePath(FStringView Path)
        {
            if (Path.size() >= 2 && Path[1] == ':')
            {
                return true;
            }
            return !Path.empty() && (Path[0] == '/' || Path[0] == '\\');
        }

        bool WriteTextFile(FStringView Path, const FString& Text)
        {
            return Filesystem::WriteFile(Path,
                TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.data()), Text.size()));
        }

        FString Slashed(FStringView Path)
        {
            FString Result(Path.data(), Path.size());
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

        struct FResolutionCacheEntry
        {
            FString          UnitName;
            FString          Fingerprint;
            TVector<FString> Assemblies;
        };

        // Spares every reload, packaging pass, and project generation a re-read of each unit's assets file.
        TVector<FResolutionCacheEntry> GResolutionCache;

        const FResolutionCacheEntry* FindCachedResolution(FStringView UnitName, const FString& Fingerprint)
        {
            for (const FResolutionCacheEntry& Entry : GResolutionCache)
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
            for (FResolutionCacheEntry& Entry : GResolutionCache)
            {
                if (Entry.UnitName == UnitName)
                {
                    Entry.Fingerprint = Fingerprint;
                    Entry.Assemblies  = Assemblies;
                    return;
                }
            }
            GResolutionCache.push_back({ FString(UnitName), Fingerprint, Assemblies });
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
        void CollectLibraryAssets(const nlohmann::json& Library, TVector<std::string>& Out)
        {
            const char* Sections[] = { "runtime", "compile" };
            for (const char* Section : Sections)
            {
                auto It = Library.find(Section);
                if (It == Library.end() || !It->is_object())
                {
                    continue;
                }
                for (auto Entry = It->begin(); Entry != It->end(); ++Entry)
                {
                    const std::string& Relative = Entry.key();
                    if (Relative.size() >= 3 && Relative.compare(Relative.size() - 3, 3, "_._") == 0)
                    {
                        continue;
                    }
                    Out.push_back(Relative);
                }
                if (!Out.empty())
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

            nlohmann::json J;
            try
            {
                J = nlohmann::json::parse(Text.c_str(), Text.c_str() + Text.size());
            }
            catch (const std::exception& Error)
            {
                LOG_ERROR("C#: failed to parse '{}': {}", AssetsPath, Error.what());
                return false;
            }

            TVector<FString> Folders;
            if (auto It = J.find("packageFolders"); It != J.end() && It->is_object())
            {
                for (auto Entry = It->begin(); Entry != It->end(); ++Entry)
                {
                    Folders.push_back(FString(Entry.key().c_str(), Entry.key().size()));
                }
            }
            if (Folders.empty())
            {
                LOG_ERROR("C#: '{}' names no package folder; cannot resolve packages.", AssetsPath);
                return false;
            }

            auto Libraries = J.find("libraries");
            auto Targets   = J.find("targets");
            if (Targets == J.end() || !Targets->is_object())
            {
                return false;
            }

            for (auto Target = Targets->begin(); Target != Targets->end(); ++Target)
            {
                if (!Target.value().is_object())
                {
                    continue;
                }
                for (auto Entry = Target.value().begin(); Entry != Target.value().end(); ++Entry)
                {
                    if (!Entry.value().is_object() || Entry.value().value("type", std::string()) != "package")
                    {
                        continue;
                    }

                    // The library's own folder name, which differs from the target key by case.
                    std::string Relative = Entry.key();
                    if (Libraries != J.end() && Libraries->is_object())
                    {
                        if (auto Library = Libraries->find(Entry.key()); Library != Libraries->end())
                        {
                            Relative = Library->value("path", Relative);
                        }
                    }

                    TVector<std::string> Assets;
                    CollectLibraryAssets(Entry.value(), Assets);
                    for (const std::string& Asset : Assets)
                    {
                        for (const FString& Folder : Folders)
                        {
                            const FString Candidate = JoinPath(Slashed(Folder),
                                JoinPath(FStringView(Relative.c_str(), Relative.size()),
                                         FStringView(Asset.c_str(), Asset.size())));
                            if (Filesystem::Exists(Candidate))
                            {
                                Out.push_back(Candidate);
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
            Params.append(ProjectPath.data(), ProjectPath.size());
            Params += "\"";

            FString Output;
            const int ExitCode = Platform::RunProcessAndWaitCapture(
                TEXT("dotnet"), UTF8_TO_TCHAR(Params.c_str()), nullptr,
                [&Output](FStringView Line)
                {
                    Output.append(Line.data(), Line.size());
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

        nlohmann::json J;
        try
        {
            J = nlohmann::json::parse(Text.c_str(), Text.c_str() + Text.size());
        }
        catch (const std::exception&)
        {
            return false; // the descriptor's own loader reports malformed JSON
        }
        if (!J.is_object())
        {
            return false;
        }

        auto Section = J.find("ScriptReferences");
        if (Section == J.end() || !Section->is_object())
        {
            return false;
        }

        if (auto It = Section->find("Packages"); It != Section->end() && It->is_array())
        {
            for (const nlohmann::json& Entry : *It)
            {
                if (!Entry.is_object())
                {
                    continue;
                }
                FScriptPackageRef Package;
                const std::string Name    = Entry.value("Name", std::string());
                const std::string Version = Entry.value("Version", std::string());
                if (Name.empty() || Version.empty())
                {
                    LOG_WARN("C#: '{}' declares a package with no Name or Version; skipping it.", DescriptorPath);
                    continue;
                }
                Package.Name    = FString(Name.c_str(), Name.size());
                Package.Version = FString(Version.c_str(), Version.size());
                Out.Packages.push_back(Move(Package));
            }
        }

        if (auto It = Section->find("Assemblies"); It != Section->end() && It->is_array())
        {
            for (const nlohmann::json& Entry : *It)
            {
                if (!Entry.is_string())
                {
                    continue;
                }
                const std::string& Value = Entry.get_ref<const std::string&>();
                const FStringView  View(Value.c_str(), Value.size());
                Out.Assemblies.push_back(IsAbsolutePath(View) ? Slashed(View) : Slashed(JoinPath(RootDir, View)));
            }
        }

        return !Out.IsEmpty();
    }

    bool ResolveScriptReferences(FStringView UnitName, FStringView RestoreDir,
        const FScriptReferenceSet& Declared, TVector<FString>& OutAssemblyPaths)
    {
        const FString CacheKey = DeclarationFingerprint(Declared);
        if (const FResolutionCacheEntry* Cached = FindCachedResolution(UnitName, CacheKey))
        {
            for (const FString& Assembly : Cached->Assemblies)
            {
                OutAssemblyPaths.push_back(Assembly);
            }
            return true;
        }

        TVector<FString> Resolved;
        for (const FString& Assembly : Declared.Assemblies)
        {
            if (Filesystem::Exists(Assembly))
            {
                Resolved.push_back(Assembly);
            }
            else
            {
                LOG_ERROR("C#: unit '{}' references '{}', which does not exist.", UnitName, Assembly);
            }
        }

        if (!Declared.Packages.empty())
        {
            if (RestoreDir.empty())
            {
                LOG_ERROR("C#: unit '{}' declares packages but has no intermediate directory to restore into.", UnitName);
                return false;
            }

            Filesystem::MakeDirectoryTree(RestoreDir);

            const FString ProjectPath  = JoinPath(RestoreDir, FString(UnitName) + ".Packages.csproj");
            const FString AssetsPath   = JoinPath(RestoreDir, "obj/project.assets.json");
            const FString StampPath    = JoinPath(RestoreDir, "packages.stamp");
            const FString Stamp        = PackageFingerprint(Declared);

            FString Previous;
            const bool bStampMatches = Filesystem::ReadFile(Previous, StampPath) && Previous == Stamp;
            if (!bStampMatches || !Filesystem::Exists(AssetsPath))
            {
                WriteTextFile(ProjectPath, BuildRestoreProject(Declared));

                LOG_DISPLAY("C#: restoring {} package(s) for unit '{}'...", Declared.Packages.size(), UnitName);
                if (!RunRestore(UnitName, ProjectPath))
                {
                    return false;
                }
                WriteTextFile(StampPath, Stamp);
            }

            const size_t Before = Resolved.size();
            if (!ReadAssetPaths(AssetsPath, Resolved))
            {
                LOG_ERROR("C#: unit '{}' restored but its package assets could not be read.", UnitName);
                return false;
            }
            LOG_DISPLAY("C#: unit '{}' resolved {} package assembly(ies).", UnitName, Resolved.size() - Before);
        }

        StoreCachedResolution(UnitName, CacheKey, Resolved);
        for (const FString& Assembly : Resolved)
        {
            OutAssemblyPaths.push_back(Assembly);
        }
        return true;
    }
}
