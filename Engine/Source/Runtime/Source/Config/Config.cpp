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

        const FString& FilePath = Class->GetMeta("ConfigFile");
        FString Result;
        if (!VFS::ReadFile(Result, FilePath))
        {
            return; // not available yet; retry on a later pass
        }

        FJsonValue FileJson;
        FJsonParseError Error;
        if (!FJsonValue::Parse(Result.View(), FileJson, &Error))
        {
            LOG_ERROR("FConfig: failed to parse settings file {0} ({1})", *FilePath, Error.Message);
            FileJson = FJsonValue::MakeObject();
        }

        FJsonValue& Cached = SettingsFileCache.Add(FilePath, Move(FileJson));

        const FString Section = GetSettingsSection(Class);
        if (FJsonValue* SectionNode = Cached.Find(Section.View()))
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

    FJsonValue& FConfig::LoadSettingsFile(const FString& VFSPath)
    {
        auto It = SettingsFileCache.Find(VFSPath);
        if (It != nullptr)
        {
            return (*It);
        }

        FJsonValue Parsed = FJsonValue::MakeObject();
        FString Result;
        if (VFS::ReadFile(Result, VFSPath))
        {
            FJsonParseError Error;
            if (!FJsonValue::Parse(Result.View(), Parsed, &Error))
            {
                LOG_ERROR("FConfig: failed to parse settings file {0} ({1})", *VFSPath, Error.Message);
                Parsed = FJsonValue::MakeObject();
            }
        }

        return SettingsFileCache.FindOrAdd(VFSPath, Move(Parsed));
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
        FJsonValue& FileJson = LoadSettingsFile(FilePath);
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

    namespace
    {
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

            FString Result;
            if (!VFS::ReadFile(Result, Info.VirtualPath))
            {
                return;
            }

            FJsonValue Parsed;
            FJsonParseError Error;
            if (!FJsonValue::Parse(Result.View(), Parsed, &Error))
            {
                LOG_ERROR("FConfig: failed to parse {0} ({1})", *Info.VirtualPath, Error.Message);
                return;
            }

            FString FilePath(*Info.VirtualPath, Info.VirtualPath.Len());
            IndexPathsForFile(Parsed, FString(), FilePath);

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

            FileConfigs.Add(FilePath, Move(Parsed));
        });
    }

    void FConfig::IndexPathsForFile(const FJsonValue& Obj, const FString& Prefix, const FString& File)
    {
        for (const auto& [Name, Value] : Obj.Members())
        {
            const FString Key = Name.ToString();
            const FString FullPath = Prefix.IsEmpty() ? Key : (Prefix + "." + Key);

            PathToFile.Add(FullPath, File);

            if (Value.IsObject())
            {
                IndexPathsForFile(Value, FullPath, File);
            }
        }
    }

    const FJsonValue* FConfig::GetRaw(FStringView Key) const
    {
        return NavigateToNode(Key);
    }

    bool FConfig::SetRaw(FStringView Key, FJsonValue Value)
    {
        TVector<FString> Parts;
        if (!SplitDottedKey(Key, Parts))
        {
            return false;
        }

        FString OwnerFile = FindOwnerFile(Key);
        if (OwnerFile.IsEmpty())
        {
            LOG_WARN("FConfig::Set: no owner file for '{0}' (register the setting first or load a config file containing it)", *FString(Key.GetData(), Key.Len()));
            return false;
        }

        // Every level but the last is forced to an object, replacing whatever scalar stood in the way.
        const auto SetPath = [&](FJsonValue& Root, FJsonValue Leaf)
        {
            if (!Root.IsObject())
            {
                Root = FJsonValue::MakeObject();
            }
            FJsonValue* Current = &Root;
            for (int32 i = 0; i + 1 < Parts.Num(); ++i)
            {
                FJsonValue* Next = Current->Find(Parts[i].View());
                if (Next == nullptr || !Next->IsObject())
                {
                    Next = &Current->Set(Parts[i].View(), FJsonValue::MakeObject());
                }
                Current = Next;
            }
            Current->Set(Parts.Last().View(), Move(Leaf));
        };

        SetPath(RootConfig, Value);

        // Mirror into the owner file's tree, then flush.
        FJsonValue& File = FileConfigs.FindOrAdd(OwnerFile);
        SetPath(File, Move(Value));

        // Track the new key so future Get->Set round-trips know its owner.
        PathToFile.Add(FString(Key.GetData(), Key.Len()), OwnerFile);

        return VFS::WriteFile(OwnerFile, File.ToString(4).View());
    }

    FString FConfig::FindOwnerFile(FStringView Key) const
    {
        // Did this exact key (or any ancestor) come from a known file?
        FString KeyStr(Key.GetData(), Key.Len());
        auto It = PathToFile.Find(KeyStr);
        if (It != nullptr)
        {
            return (*It);
        }

        FString Cursor = KeyStr;
        int32 LastDot = Cursor.FindLastOf('.');
        while (LastDot != Constants::kIndexNone)
        {
            Cursor = Cursor.Mid(0, LastDot);
            It = PathToFile.Find(Cursor);
            if (It != nullptr)
            {
                return (*It);
            }
            LastDot = Cursor.FindLastOf('.');
        }

        return FString();
    }

    FJsonValue* FConfig::NavigateToNode(FStringView Key, bool bCreate)
    {
        TVector<FString> Parts;
        if (!SplitDottedKey(Key, Parts))
        {
            return nullptr;
        }

        FJsonValue* Current = &RootConfig;
        for (int32 i = 0; i < Parts.Num(); ++i)
        {
            const bool bLast = (i + 1 == Parts.Num());

            if (FJsonValue* Next = Current->Find(Parts[i].View()))
            {
                Current = Next;
                continue;
            }

            if (!bCreate || !(Current->IsObject() || Current->IsNull()))
            {
                return nullptr;
            }

            Current = &Current->Set(Parts[i].View(), bLast ? FJsonValue() : FJsonValue::MakeObject());
        }
        return Current;
    }

    const FJsonValue* FConfig::NavigateToNode(FStringView Key) const
    {
        return const_cast<FConfig*>(this)->NavigateToNode(Key, false);
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
