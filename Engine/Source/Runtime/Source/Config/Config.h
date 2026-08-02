#pragma once

#include "Core/Math/Math.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Templates/LuminaTemplate.h"
#include "nlohmann/json.hpp"

namespace Lumina
{
    class CClass;
    class CObject;

    RUNTIME_API extern class FConfig* GConfig;

    class RUNTIME_API FConfig
    {
    public:

        // ---- Developer settings (reflection-driven CDeveloperSettings objects) ----

        // Discover every CDeveloperSettings subclass and load its JSON section into its CDO.
        // Idempotent: classes already initialized are skipped, so it is safe to call again after
        // a module (e.g. the editor) registers more settings classes.
        void DiscoverAndLoadSettings();

        // Force re-read of one already-loaded settings class's file section into its CDO. Use after new classes
        // become available so class-reference properties (TSubclassOf) re-resolve -- e.g. a C# script reload mints
        // a CGameInstance subclass that Project.GameInstanceClass names but couldn't resolve at first load.
        void ReloadSettings(CClass* SettingsClass);

        // Re-serialize a settings class's CDO into its ConfigFile, preserving other sections.
        void SaveSettings(CClass* SettingsClass);

        // Iterate discovered settings classes in load order (for the editor panel).
        void ForEachSettingsClass(const TFunction<void(CClass*)>& Func) const;

        // Pristine code-default snapshot for a settings class (reset-to-default baseline). Null if none.
        CObject* GetSettingsDefault(CClass* SettingsClass) const;

        // Section key a class persists under within its grouped ConfigFile (class name sans leading 'C').
        static FString GetSettingsSection(CClass* SettingsClass);

        // ---- Generic JSON config (used for input-action maps + cooked/launch metadata) ----

        // Loads every .json under the VFS dir; top-level keys merge into root, source path recorded so Set() writes back.
        void LoadPath(FStringView ConfigPath);

        // Typed read of a dotted key; missing key returns the Default arg.
        template<typename T>
        T Get(FStringView Key, const T& Default = T{}) const;

        // Generic typed write. Updates the in-memory tree and writes the
        // owning file to disk. Returns false if no owner file can be inferred.
        template<typename T>
        bool Set(FStringView Key, const T& Value);

        // Raw JSON access for advanced cases (e.g. the input-action map loader).
        const nlohmann::json* GetRaw(FStringView Key) const;

    private:

        // Reads one (already-discovered) settings class's file section into its CDO and marks it loaded. The
        // caller decides whether to skip already-loaded classes (DiscoverAndLoadSettings) or force (ReloadSettings).
        void LoadSettingsForClass(CClass* Class);

        // Loads (and caches) a settings JSON file; returns an empty object if absent/unparseable.
        nlohmann::json& LoadSettingsFile(const FString& VFSPath);

        bool SetRaw(FStringView Key, nlohmann::json Value);

        nlohmann::json* NavigateToNode(FStringView Key, bool bCreate);
        const nlohmann::json* NavigateToNode(FStringView Key) const;

        // Resolves which file owns Key: explicit registration first,
        // then existing PathToFile entry, else "".
        FString FindOwnerFile(FStringView Key) const;

        // Walk a JSON subtree and record every leaf path under the file it lives in.
        void IndexPathsForFile(const nlohmann::json& Obj, const FString& Prefix, const FString& File);

        nlohmann::json                              RootConfig;
        THashMap<FString, nlohmann::json>           FileConfigs;        // VFS path -> parsed json
        THashMap<FString, FString>                  PathToFile;         // dotted-key prefix -> VFS path

        // Developer-settings state.
        THashMap<CClass*, CObject*>                 SettingsDefaults;   // class -> pristine code-default snapshot
        TVector<CClass*>                            DiscoveredSettings; // discovery/load order
        THashSet<CClass*>                           SettingsFileLoaded; // classes whose file values were applied
        THashMap<FString, nlohmann::json>           SettingsFileCache;  // VFS path -> parsed settings file
    };

    // Template definitions.

    template<typename T>
    T FConfig::Get(FStringView Key, const T& Default) const
    {
        const nlohmann::json* Node = NavigateToNode(Key);
        if (Node != nullptr)
        {
            try
            {
                return Node->get<T>();
            }
            catch (...)
            {
                // Fall through to default, value type didn't match.
            }
        }

        return Default;
    }

    template<typename T>
    bool FConfig::Set(FStringView Key, const T& Value)
    {
        return SetRaw(Key, nlohmann::json(Value));
    }
}
