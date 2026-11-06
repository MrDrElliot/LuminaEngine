#include "RuntimePCH.h"
#include "Config.h"
#include "DeveloperSettings.h"

#include "Core/Math/Math.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"

#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    RUNTIME_API FConfig* GConfig;

    namespace
    {
        // False only when the file cannot be read; an unparseable file logs and yields an empty object.
        bool ReadJsonFile(const FString& VFSPath, FJsonValue& Out)
        {
            FString Result;
            if (!VFS::ReadFile(Result, VFSPath))
            {
                return false;
            }

            FJsonParseError Error;
            if (!FJsonValue::Parse(Result.View(), Out, &Error))
            {
                LOG_ERROR("FConfig: failed to parse {0} ({1})", *VFSPath, Error.Message);
                Out = FJsonValue::MakeObject();
            }
            return true;
        }

        // Returns false and leaves Out empty for an empty key.
        bool SplitDottedKey(FStringView Key, TVector<FString>& Out)
        {
            Out.Reset();
            if (Key.IsEmpty())
            {
                return false;
            }

            size_t Start = 0;
            for (int32 i = 0; i < Key.Len(); ++i)
            {
                if (Key[i] == '.')
                {
                    Out.Emplace(Key.GetData() + Start, Key.GetData() + i);
                    Start = i + 1;
                }
            }
            Out.Emplace(Key.GetData() + Start, Key.GetData() + Key.Len());
            return !Out.IsEmpty();
        }
    }

    void FConfig::DiscoverAndLoadSettings()
    {
        CClass* BaseClass = CDeveloperSettings::StaticClass();

        // Collect candidates first; creating CDOs/snapshots mutates the object array.
        TVector<CClass*> Candidates;
        ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class != BaseClass && Class->IsChildOf(BaseClass))
            {
                Candidates.Add(Class);
            }
        });

        // 1) Discover + snapshot pristine code defaults for any class not seen yet.
        for (CClass* Class : Candidates)
        {
            if (SettingsDefaults.Contains(Class))
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
            DiscoveredSettings.Add(Class);
        }
        
        for (CClass* Class : DiscoveredSettings)
        {
            if (SettingsFileLoaded.Contains(Class))
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
            SettingsFileLoaded.Add(Class);
            return;
        }

        FJsonValue* FileJson = FindOrLoadSettingsFile(Class->GetMeta("ConfigFile"));
        if (FileJson == nullptr)
        {
            return; // not available yet; retry on a later pass
        }

        const FString Section = GetSettingsSection(Class);
        if (FJsonValue* SectionNode = FileJson->Find(Section.View()))
        {
            FJsonStructuredArchive::LoadStruct(*SectionNode, Class, Class->GetDefaultObject());
        }

        static_cast<CDeveloperSettings*>(Class->GetDefaultObject())->PostInitSettings();
        SettingsFileLoaded.Add(Class);
    }

    FString FConfig::GetSettingsSection(CClass* SettingsClass)
    {
        FString Name = SettingsClass->GetName().ToString();
        // Drop the conventional leading 'C' for tidy, stable JSON keys (e.g. "WorldEditorSettings").
        if (Name.Len() > 1 && Name[0] == 'C')
        {
            return Name.Mid(1);
        }
        return Name;
    }

    FJsonValue* FConfig::FindOrLoadSettingsFile(const FString& VFSPath)
    {
        if (FJsonValue* Cached = SettingsFileCache.Find(VFSPath))
        {
            return Cached;
        }

        FJsonValue Parsed;
        if (!ReadJsonFile(VFSPath, Parsed))
        {
            return nullptr;
        }
        return &SettingsFileCache.Add(VFSPath, Move(Parsed));
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
        FJsonValue* Loaded = FindOrLoadSettingsFile(FilePath);
        FJsonValue& FileJson = Loaded != nullptr ? *Loaded : SettingsFileCache.Add(FilePath, FJsonValue::MakeObject());
        if (!FileJson.IsObject())
        {
            FileJson = FJsonValue::MakeObject();
        }

        const FString Section = GetSettingsSection(SettingsClass);
        FJsonStructuredArchive::SaveStruct(FileJson.FindOrAdd(Section.View()), SettingsClass, CDO);

        const FString Dumped = FileJson.ToString(4);
        if (!VFS::WriteFile(FilePath, Dumped.View()))
        {
            LOG_ERROR("FConfig: failed to write settings file {0}", *FilePath);
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
        auto It = SettingsDefaults.Find(SettingsClass);
        return It == nullptr ? nullptr : (*It);
    }

    void FConfig::LoadPath(FStringView ConfigPath)
    {
        // Iterating a missing directory makes the VFS warn on every first run, so ask first.
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

            FJsonValue Parsed;
            if (!ReadJsonFile(FString(*Info.VirtualPath, Info.VirtualPath.Len()), Parsed))
            {
                return;
            }

            // Later files take precedence, which is the intended project-overrides-engine layering.
            for (const FJsonMember& Member : Parsed.Members())
            {
                FJsonValue* Existing = RootConfig.Find(Member.Key.View());
                if (Member.Value.IsObject() && Existing != nullptr && Existing->IsObject())
                {
                    Existing->MergePatch(Member.Value);
                }
                else
                {
                    RootConfig.Set(Member.Key.View(), Member.Value);
                }
            }
        });
    }

    const FJsonValue* FConfig::NavigateToNode(FStringView Key) const
    {
        TVector<FString> Parts;
        if (!SplitDottedKey(Key, Parts))
        {
            return nullptr;
        }

        const FJsonValue* Current = &RootConfig;
        for (const FString& Part : Parts)
        {
            Current = Current->Find(Part.View());
            if (Current == nullptr)
            {
                return nullptr;
            }
        }
        return Current;
    }

}

namespace Lumina
{
    void FConfig::VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        // Keyed by class, so a repointed key means a rebuilt table rather than an in-place write: the old
        // key would keep the bucket its old hash chose and the entry would never be found again.
        THashMap<CClass*, CObject*> RebuiltDefaults;
        RebuiltDefaults.Reserve(SettingsDefaults.Num());
        for (const auto& [Class, Snapshot] : SettingsDefaults)
        {
            auto* const NewClass = Cast<CClass>(Func(Class));
            CObject* const NewSnapshot = Func(Snapshot);
            if (NewClass != nullptr)
            {
                RebuiltDefaults.Add(NewClass, NewSnapshot);
            }
        }
        SettingsDefaults = Move(RebuiltDefaults);

        for (CClass*& Class : DiscoveredSettings)
        {
            Class = Cast<CClass>(Func(Class));
        }

        THashSet<CClass*> RebuiltLoaded;
        RebuiltLoaded.Reserve(SettingsFileLoaded.Num());
        for (CClass* Class : SettingsFileLoaded)
        {
            if (auto* const Replacement = Cast<CClass>(Func(Class)))
            {
                RebuiltLoaded.Add(Replacement);
            }
        }
        SettingsFileLoaded = Move(RebuiltLoaded);
    }
}
