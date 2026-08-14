#include "RuntimePCH.h"
#include "Config.h"
#include "DeveloperSettings.h"

#include "Core/Math/Math.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"

#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

using Json = nlohmann::json;

namespace Lumina
{
    RUNTIME_API FConfig* GConfig;

    void FConfig::DiscoverAndLoadSettings()
    {
        CClass* BaseClass = CDeveloperSettings::StaticClass();

        // Collect candidates first; creating CDOs/snapshots mutates the object array.
        TVector<CClass*> Candidates;
        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class != BaseClass && Class->IsChildOf(BaseClass))
            {
                Candidates.push_back(Class);
            }
        });

        // 1) Discover + snapshot pristine code defaults for any class not seen yet.
        for (CClass* Class : Candidates)
        {
            if (SettingsDefaults.find(Class) != SettingsDefaults.end())
            {
                continue;
            }

            if (Class->GetDefaultObject() == nullptr)
            {
                continue;
            }

            CObject* Snapshot = NewObject<CObject>(Class, nullptr, NAME_None, FGuid::New(), OF_Transient);
            if (Snapshot)
            {
                Snapshot->AddToRoot();
            }
            SettingsDefaults[Class] = Snapshot;
            DiscoveredSettings.push_back(Class);
        }
        
        for (CClass* Class : DiscoveredSettings)
        {
            if (SettingsFileLoaded.find(Class) != SettingsFileLoaded.end())
            {
                continue;
            }
            LoadSettingsForClass(Class);
        }
    }

    void FConfig::LoadSettingsForClass(CClass* Class)
    {
        if (!Class->HasMeta("ConfigFile"))
        {
            SettingsFileLoaded.insert(Class);
            return;
        }

        const FString& FilePath = Class->GetMeta("ConfigFile");
        FString Result;
        if (!VFS::ReadFile(Result, FilePath))
        {
            return; // not available yet; retry on a later pass
        }

        Json FileJson;
        try
        {
            FileJson = Json::parse(Result.c_str());
        }
        catch (const std::exception& Ex)
        {
            LOG_ERROR("FConfig: failed to parse settings file {0} ({1})", FilePath.c_str(), Ex.what());
            FileJson = Json::object();
        }

        SettingsFileCache[FilePath] = FileJson;

        const FString Section = GetSettingsSection(Class);
        auto SectionIt = FileJson.find(Section.c_str());
        if (SectionIt != FileJson.end())
        {
            FJsonStructuredArchive::LoadStruct(*SectionIt, Class, Class->GetDefaultObject());
        }

        static_cast<CDeveloperSettings*>(Class->GetDefaultObject())->PostInitSettings();
        SettingsFileLoaded.insert(Class);
    }

    void FConfig::ReloadSettings(CClass* SettingsClass)
    {
        if (SettingsClass == nullptr)
        {
            return;
        }
        // Force a re-read even if already loaded; class-reference properties may now resolve to classes that
        // didn't exist at first load (e.g. C# Scriptable subclasses minted during script (re)load).
        SettingsFileLoaded.erase(SettingsClass);
        LoadSettingsForClass(SettingsClass);
    }

    FString FConfig::GetSettingsSection(CClass* SettingsClass)
    {
        FString Name = SettingsClass->GetName().ToString();
        // Drop the conventional leading 'C' for tidy, stable JSON keys (e.g. "WorldEditorSettings").
        if (Name.size() > 1 && Name[0] == 'C')
        {
            return Name.substr(1);
        }
        return Name;
    }

    Json& FConfig::LoadSettingsFile(const FString& VFSPath)
    {
        auto It = SettingsFileCache.find(VFSPath);
        if (It != SettingsFileCache.end())
        {
            return It->second;
        }

        Json Parsed = Json::object();
        FString Result;
        if (VFS::ReadFile(Result, VFSPath))
        {
            try
            {
                Parsed = Json::parse(Result.c_str());
            }
            catch (const std::exception& Ex)
            {
                LOG_ERROR("FConfig: failed to parse settings file {0} ({1})", VFSPath.c_str(), Ex.what());
                Parsed = Json::object();
            }
        }

        return SettingsFileCache.emplace(VFSPath, Move(Parsed)).first->second;
    }

    void FConfig::SaveSettings(CClass* SettingsClass)
    {
        if (SettingsClass == nullptr || !SettingsClass->HasMeta("ConfigFile"))
        {
            return;
        }

        CObject* CDO = SettingsClass->GetDefaultObject();
        if (CDO == nullptr)
        {
            return;
        }

        const FString& FilePath = SettingsClass->GetMeta("ConfigFile");
        Json& FileJson = LoadSettingsFile(FilePath);

        const FString Section = GetSettingsSection(SettingsClass);
        FJsonStructuredArchive::SaveStruct(FileJson[Section.c_str()], SettingsClass, CDO);

        const std::string Dumped = FileJson.dump(4);
        if (!VFS::WriteFile(FilePath, FStringView(Dumped.c_str(), Dumped.size())))
        {
            LOG_ERROR("FConfig: failed to write settings file {0}", FilePath.c_str());
        }

        // Let open editors live-refresh from the just-saved values.
        FCoreDelegates::OnSettingsSaved.Broadcast(SettingsClass);
    }

    void FConfig::ForEachSettingsClass(const TFunction<void(CClass*)>& Func) const
    {
        for (CClass* Class : DiscoveredSettings)
        {
            Func(Class);
        }
    }

    CObject* FConfig::GetSettingsDefault(CClass* SettingsClass) const
    {
        auto It = SettingsDefaults.find(SettingsClass);
        return It == SettingsDefaults.end() ? nullptr : It->second;
    }

    namespace
    {
        // Split a dotted key like "Editor.WorldEditorTool.GuizmoSnapEnabled"
        // into its segments. Returns false (and leaves Out empty) for empty Key.
        bool SplitDottedKey(FStringView Key, TVector<FString>& Out)
        {
            Out.clear();
            if (Key.empty())
            {
                return false;
            }

            size_t Start = 0;
            for (size_t i = 0; i < Key.size(); ++i)
            {
                if (Key[i] == '.')
                {
                    Out.emplace_back(Key.data() + Start, Key.data() + i);
                    Start = i + 1;
                }
            }
            Out.emplace_back(Key.data() + Start, Key.data() + Key.size());
            return !Out.empty();
        }
    }

    void FConfig::LoadPath(FStringView ConfigPath)
    {
        // A config directory that does not exist yet holds no settings, which is the correct state
        // for a clean clone: the directory appears when something is first saved into it. Iterating
        // it anyway makes the VFS warn about a missing directory on every first run, so ask first.
        if (!VFS::Exists(ConfigPath))
        {
            return;
        }

        VFS::DirectoryIterator(ConfigPath, [&](const VFS::FFileInfo& Info)
        {
            if (Info.GetExt() != ".json")
            {
                return;
            }

            FString Result;
            if (!VFS::ReadFile(Result, Info.VirtualPath))
            {
                return;
            }

            Json Parsed;
            try
            {
                Parsed = Json::parse(Result.c_str());
            }
            catch (const std::exception& Ex)
            {
                LOG_ERROR("FConfig: failed to parse {0} ({1})", Info.VirtualPath.c_str(), Ex.what());
                return;
            }

            FString FilePath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
            FileConfigs[FilePath] = Parsed;

            IndexPathsForFile(Parsed, FString(), FilePath);

            // Merge into the root tree. Later files take precedence, this is
            // intentional for project-overrides-engine layering, the same as before.
            for (auto It = Parsed.begin(); It != Parsed.end(); ++It)
            {
                if (It->is_object() && RootConfig.contains(It.key()) && RootConfig[It.key()].is_object())
                {
                    RootConfig[It.key()].merge_patch(*It);
                }
                else
                {
                    RootConfig[It.key()] = *It;
                }
            }
        });
    }

    void FConfig::IndexPathsForFile(const Json& Obj, const FString& Prefix, const FString& File)
    {
        if (!Obj.is_object())
        {
            return;
        }

        for (auto It = Obj.begin(); It != Obj.end(); ++It)
        {
            FString Key = It.key().c_str();
            FString FullPath = Prefix.empty() ? Key : (Prefix + "." + Key);

            PathToFile[FullPath] = File;

            if (It->is_object())
            {
                IndexPathsForFile(*It, FullPath, File);
            }
        }
    }

    const Json* FConfig::GetRaw(FStringView Key) const
    {
        return NavigateToNode(Key);
    }

    bool FConfig::SetRaw(FStringView Key, Json Value)
    {
        TVector<FString> Parts;
        if (!SplitDottedKey(Key, Parts))
        {
            return false;
        }

        FString OwnerFile = FindOwnerFile(Key);
        if (OwnerFile.empty())
        {
            LOG_WARN("FConfig::Set: no owner file for '{0}' (register the setting first or load a config file containing it)", FString(Key.data(), Key.size()).c_str());
            return false;
        }

        // Update the unified tree.
        Json* Current = &RootConfig;
        for (size_t i = 0; i + 1 < Parts.size(); ++i)
        {
            const std::string PartStr = Parts[i].c_str();
            if (!Current->contains(PartStr) || !(*Current)[PartStr].is_object())
            {
                (*Current)[PartStr] = Json::object();
            }
            Current = &(*Current)[PartStr];
        }
        (*Current)[Parts.back().c_str()] = Value;

        // Mirror into the owner file's tree, then flush.
        Json& File = FileConfigs[OwnerFile];
        Current = &File;
        for (size_t i = 0; i + 1 < Parts.size(); ++i)
        {
            const std::string PartStr = Parts[i].c_str();
            if (!Current->contains(PartStr) || !(*Current)[PartStr].is_object())
            {
                (*Current)[PartStr] = Json::object();
            }
            Current = &(*Current)[PartStr];
        }
        (*Current)[Parts.back().c_str()] = Move(Value);

        // Track the new key so future Get->Set round-trips know its owner.
        PathToFile[FString(Key.data(), Key.size())] = OwnerFile;

        const std::string Dumped = File.dump(4);
        return VFS::WriteFile(OwnerFile, FStringView(Dumped.c_str(), Dumped.size()));
    }

    FString FConfig::FindOwnerFile(FStringView Key) const
    {
        // Did this exact key (or any ancestor) come from a known file?
        FString KeyStr(Key.data(), Key.size());
        auto It = PathToFile.find(KeyStr);
        if (It != PathToFile.end())
        {
            return It->second;
        }

        FString Cursor = KeyStr;
        size_t LastDot = Cursor.find_last_of('.');
        while (LastDot != FString::npos)
        {
            Cursor = Cursor.substr(0, LastDot);
            It = PathToFile.find(Cursor);
            if (It != PathToFile.end())
            {
                return It->second;
            }
            LastDot = Cursor.find_last_of('.');
        }

        return FString();
    }

    Json* FConfig::NavigateToNode(FStringView Key, bool bCreate)
    {
        TVector<FString> Parts;
        if (!SplitDottedKey(Key, Parts))
        {
            return nullptr;
        }

        Json* Current = &RootConfig;
        for (size_t i = 0; i < Parts.size(); ++i)
        {
            const std::string PartStr = Parts[i].c_str();
            const bool bLast = (i + 1 == Parts.size());

            if (Current->contains(PartStr))
            {
                Current = &(*Current)[PartStr];
                continue;
            }

            if (!bCreate)
            {
                return nullptr;
            }

            (*Current)[PartStr] = bLast ? Json() : Json::object();
            Current = &(*Current)[PartStr];
        }
        return Current;
    }

    const Json* FConfig::NavigateToNode(FStringView Key) const
    {
        return const_cast<FConfig*>(this)->NavigateToNode(Key, false);
    }

}
