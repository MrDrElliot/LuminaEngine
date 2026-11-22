#pragma once

#include "Plugin.h"
#include "PluginLoadingPhase.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Per-project plugin enable/disable from the .lproject "Plugins" array, overriding the descriptor default.
    struct FProjectPluginOverride
    {
        FString Name;
        bool    bEnabled = true;
    };

    // Discovers .lplugin files, resolves deps, mounts content, and loads module DLLs per declared phase.
    class RUNTIME_API FPluginManager
    {
    public:
        // A synthesized copy ctor would fail on TUniquePtr's deleted copy.
        LE_NO_COPYMOVE(FPluginManager);

        static FPluginManager& Get();

        // Builds the engine-plugin registry from Engine/Plugins/. Idempotent.
        void DiscoverEnginePlugins();

        // Adds project plugins, which may depend on engine plugins. Idempotent for the same project root.
        void DiscoverProjectPlugins(FStringView ProjectDirectory);

        // Applies per-project enable/disable overrides right after DiscoverProjectPlugins.
        void ApplyProjectOverrides(const TVector<FProjectPluginOverride>& Overrides);

        // Bring up every enabled plugin module registered to this phase; already-loaded skip.
        void LoadModulesForPhase(EPluginLoadingPhase Phase);

        // Reverse-order shutdown that must run before the module manager unloads anything else.
        void ShutdownAllPlugins();

        FPlugin*               FindPlugin(FStringView Name);
        const FPlugin*         FindPlugin(FStringView Name) const;
        TVector<FPlugin*>      GetEnabledPlugins();
        TVector<const FPlugin*> GetAllPlugins() const;

    private:
        FPluginManager() = default;
        ~FPluginManager() = default;

        // Discover .lplugin files under Root; bIsEngine tags them as engine plugins.
        void DiscoverDirectory(FStringView Root, bool bIsEngine);

        // Load one plugin module: resolve binary path, FModuleManager, StartupModule, record.
        bool LoadPluginModule(FPlugin& Plugin, const FPluginModuleDescriptor& Module);

        // Skipped when the plugin has no content or its Content/ dir is missing.
        void MountPluginContent(FPlugin& Plugin);

        // False means skip this module in this build (editor module in non-editor, developer in shipping, or platform mismatch).
        bool IsModuleApplicable(const FPluginModuleDescriptor& Module) const;

        // Plugin-level filter that skips the whole plugin on editor-only or platform mismatch.
        bool IsPluginApplicable(const FPluginDescriptor& Desc) const;

        // Topological sort, deterministic because peers and cycles fall back to insertion order.
        TVector<FPlugin*> BuildLoadOrder();

    private:
        // The hash map eagerly instantiates the value copy ctor, so the vector owns and the map only looks up.
        TVector<TUniquePtr<FPlugin>> OwnedPlugins;
        THashMap<FName, FPlugin*>    PluginLookup;

        // Cached topological order, invalidated by discovery and overrides.
        TVector<FPlugin*> CachedLoadOrder;
        bool              bLoadOrderDirty = true;

        FString           ProjectDirectory; // last DiscoverProjectPlugins arg

        // Phases LoadModulesForPhase has run, so a module discovered after its own phase still loads.
        bool              PhaseFired[(int32)EPluginLoadingPhase::Count] = {};

        // Modules already tried, so one that failed is not retried at every later phase.
        THashSet<FName>   AttemptedModules;
    };
}
