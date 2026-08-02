#pragma once

#include "Plugin.h"
#include "PluginLoadingPhase.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Per-project plugin enable/disable from the .lproject "Plugins" array,
    // overriding the descriptor default.
    struct FProjectPluginOverride
    {
        FString Name;
        bool    bEnabled = true;
    };

    // Discovers .lplugin files, resolves deps, mounts content, loads module DLLs per declared
    // phase. Engine plugins scanned in FEngine::Init, project plugins in Engine::LoadProject.
    class RUNTIME_API FPluginManager
    {
    public:
        // Non-copyable: a synthesized copy ctor would instantiate the vector's copy
        // and fail on TUniquePtr's deleted copy.
        LE_NO_COPYMOVE(FPluginManager);

        static FPluginManager& Get();

        // Walk Engine/Plugins/ (relative to Paths::GetEngineDirectory()) and
        // build the engine-plugin registry. Idempotent; safe to call twice.
        void DiscoverEnginePlugins();

        // Add project plugins from <ProjectDirectory>/Plugins/ (may depend on engine
        // plugins). Idempotent for the same project root.
        void DiscoverProjectPlugins(FStringView ProjectDirectory);

        // Apply per-project enable/disable overrides. Called once
        // immediately after DiscoverProjectPlugins from LoadProject.
        void ApplyProjectOverrides(const TVector<FProjectPluginOverride>& Overrides);

        // Bring up every enabled plugin module registered to this phase; already-loaded skip.
        void LoadModulesForPhase(EPluginLoadingPhase Phase);

        // Reverse-order shutdown of every loaded plugin module. Called from
        // FEngine::Shutdown before the module manager unloads anything else.
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

        // Mount /PluginName into the VFS. Skipped if !ContainsContent or
        // the Content/ dir does not exist on disk.
        void MountPluginContent(FPlugin& Plugin);

        // False = skip this module in this build (editor-in-non-editor, developer-in-shipping,
        // or SupportedPlatforms mismatch).
        bool IsModuleApplicable(const FPluginModuleDescriptor& Module) const;

        // Filter on the plugin level: EditorOnly + non-editor build skips
        // the whole plugin, SupportedPlatforms misses ditto.
        bool IsPluginApplicable(const FPluginDescriptor& Desc) const;

        // Topological sort so dependents load after deps; deterministic (peers and cycles
        // fall back to insertion order, cycles logged).
        TVector<FPlugin*> BuildLoadOrder();

    private:
        // Vector owns; map is O(1) name lookup. TUniquePtr can't be a map value
        // (EASTL hash_map eagerly instantiates the value copy ctor).
        TVector<TUniquePtr<FPlugin>> OwnedPlugins;
        THashMap<FName, FPlugin*>    PluginLookup;

        // Cache of the topological order so LoadModulesForPhase doesn't
        // resort on every phase. Invalidated by discovery / overrides.
        TVector<FPlugin*> CachedLoadOrder;
        bool              bLoadOrderDirty = true;

        FString           ProjectDirectory; // last DiscoverProjectPlugins arg
    };
}
