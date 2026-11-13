#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Json/Json.h"

namespace Lumina
{
    class CClass;
    class CObject;

    class FConfig;

    namespace Global
    {
        RUNTIME_API extern FConfig* GConfig;
    }

    class RUNTIME_API FConfig : public IObjectReferenceProvider
    {
    public:

        //~ IObjectReferenceProvider: the developer-settings snapshots are a plain map, so nothing else
        //~ reaches the classes and default objects they hold.

        const char* GetReferenceProviderName() const override { return "config settings defaults"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;


        // ---- Developer settings (reflection-driven CDeveloperSettings objects) ----

        // Discover every CDeveloperSettings subclass and load its JSON section into its CDO.
        // Idempotent: classes already initialized are skipped, so it is safe to call again after
        // a module (e.g. the editor) registers more settings classes.
        void DiscoverAndLoadSettings();

        // Re-serialize a settings class's CDO into its ConfigFile, preserving other sections.
        void SaveSettings(CClass* SettingsClass);

        // Iterate discovered settings classes in load order (for the editor panel).
        void ForEachSettingsClass(const TFunction<void(CClass*)>& Func) const;

        // Pristine code-default snapshot for a settings class (reset-to-default baseline). Null if none.
        CObject* GetSettingsDefault(CClass* SettingsClass) const;

        // Section key a class persists under within its grouped ConfigFile (class name sans leading 'C').
        static FString GetSettingsSection(CClass* SettingsClass);

        // ---- Generic JSON config (read-only, e.g. Project.Name) ----

        // Loads every .json under the VFS dir; top-level keys merge into root, later files overriding earlier ones.
        void LoadPath(FStringView ConfigPath);

        // Typed read of a dotted key; missing key returns the Default arg.
        template<typename T>
        T Get(FStringView Key, const T& Default = T{}) const;

    private:

        // Reads one settings class's file section into its CDO and marks it loaded, unless the file is not readable yet.
        void LoadSettingsForClass(CClass* Class);

        // Cached parse of a settings file, or null while it cannot be read; an unparseable file caches an empty object.
        FJsonValue* FindOrLoadSettingsFile(const FString& VFSPath);

        const FJsonValue* NavigateToNode(FStringView Key) const;

        FJsonValue                                  RootConfig;

        // Developer-settings state.
        THashMap<CClass*, CObject*>                 SettingsDefaults;   // class -> pristine code-default snapshot
        TVector<CClass*>                            DiscoveredSettings; // discovery/load order
        THashSet<CClass*>                           SettingsFileLoaded; // classes whose file values were applied
        THashMap<FString, FJsonValue>               SettingsFileCache;  // VFS path -> parsed settings file
    };

    // Template definitions.

    template<typename T>
    T FConfig::Get(FStringView Key, const T& Default) const
    {
        const FJsonValue* Node = NavigateToNode(Key);
        T Value;
        return Node != nullptr && Node->TryGet(Value) ? Value : Default;
    }
}
