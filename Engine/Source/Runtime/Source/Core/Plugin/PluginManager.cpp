#include "RuntimePCH.h"
#include "PluginManager.h"
#include "Plugin.h"
#include "PluginDescriptor.h"

#include "Core/Module/ModuleManager.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/NativeFileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"

#include <filesystem>

namespace Lumina
{
    FPluginManager& FPluginManager::Get()
    {
        static FPluginManager Instance;
        return Instance;
    }

    void FPluginManager::DiscoverEnginePlugins()
    {
        const FString& EngineDir = Paths::GetEngineDirectory();
        if (EngineDir.empty())
        {
            return;
        }
        FString Root = EngineDir;
        Root += "/Plugins";
        DiscoverDirectory(Root, /*bIsEngine*/true);
    }

    void FPluginManager::DiscoverProjectPlugins(FStringView ProjectDir)
    {
        if (ProjectDir.empty())
        {
            return;
        }
        ProjectDirectory.assign(ProjectDir.data(), ProjectDir.size());
        FString Root = ProjectDirectory;
        Root += "/Plugins";
        DiscoverDirectory(Root, /*bIsEngine*/false);
    }

    void FPluginManager::DiscoverDirectory(FStringView RootView, bool bIsEngine)
    {
        std::error_code EC;
        std::filesystem::path Root(RootView.data(), RootView.data() + RootView.size());
        const bool bExists = std::filesystem::exists(Root, EC);
        if (EC)
        {
            LOG_WARN("[PluginManager] exists({}) failed: {}", RootView, EC.message().c_str());
            return;
        }
        if (!bExists)
        {
            return;
        }
        if (!std::filesystem::is_directory(Root, EC) || EC)
        {
            if (EC) LOG_WARN("[PluginManager] is_directory({}) failed: {}", RootView, EC.message().c_str());
            return;
        }

        // Each subdir is a candidate plugin; prefer <SubDir>/<SubDir>.lplugin, else any .lplugin.
        auto It = std::filesystem::directory_iterator(Root, EC);
        if (EC)
        {
            LOG_WARN("[PluginManager] directory_iterator({}) failed: {}", RootView, EC.message().c_str());
            return;
        }

        for (const auto& Entry : It)
        {
            EC.clear();
            if (!Entry.is_directory(EC) || EC)
            {
                if (EC) LOG_WARN("[PluginManager] is_directory({}) failed: {}", Entry.path().string().c_str(), EC.message().c_str());
                continue;
            }

            std::filesystem::path PluginDir = Entry.path();
            std::filesystem::path Conventional = PluginDir / (PluginDir.filename().string() + ".lplugin");
            std::filesystem::path Descriptor;

            EC.clear();
            if (std::filesystem::exists(Conventional, EC))
            {
                Descriptor = Conventional;
            }
            else
            {
                EC.clear();
                auto Inner = std::filesystem::directory_iterator(PluginDir, EC);
                if (EC)
                {
                    LOG_WARN("[PluginManager] directory_iterator({}) failed: {}", PluginDir.string().c_str(), EC.message().c_str());
                    continue;
                }
                bool bFoundFallback = false;
                for (const auto& Candidate : Inner)
                {
                    EC.clear();
                    if (Candidate.is_regular_file(EC) && Candidate.path().extension() == ".lplugin")
                    {
                        Descriptor = Candidate.path();
                        bFoundFallback = true;
                        // Warn so a misnamed descriptor surfaces in the log
                        // instead of "first .lplugin wins" silently.
                        LOG_WARN("[PluginManager] {} contains a .lplugin not matching the folder name; using {} (rename to {})",
                            PluginDir.string().c_str(),
                            Descriptor.filename().string().c_str(),
                            Conventional.filename().string().c_str());
                        break;
                    }
                }
                (void)bFoundFallback;
            }

            if (Descriptor.empty())
            {
                continue;
            }

            FString DescriptorPath(Descriptor.string().c_str());
            FString PluginDirStr(PluginDir.string().c_str());
            Paths::Normalize(DescriptorPath);
            Paths::Normalize(PluginDirStr);

            FPluginDescriptor Parsed;
            FString Error;
            if (!FPluginDescriptor::LoadFromFile(DescriptorPath, Parsed, Error))
            {
                LOG_WARN("[PluginManager] Skipping plugin at {}: {}", DescriptorPath, Error);
                continue;
            }
            Parsed.bIsEnginePlugin = bIsEngine;

            // Duplicate-name detection: first one wins, later ones logged.
            FName Key(Parsed.Name);
            if (PluginLookup.find(Key) != PluginLookup.end())
            {
                LOG_WARN("[PluginManager] Duplicate plugin name '{}' at {} ignored (already registered)",
                    Parsed.Name, DescriptorPath);
                continue;
            }

            LOG_INFO("[PluginManager] Discovered {} plugin '{}' ({} modules)",
                bIsEngine ? "engine" : "project", Parsed.Name, Parsed.Modules.size());

            TUniquePtr<FPlugin> Owned = MakeUnique<FPlugin>(Move(Parsed), Move(PluginDirStr), Move(DescriptorPath));
            FPlugin* Raw = Owned.get();
            OwnedPlugins.emplace_back(Move(Owned));
            PluginLookup.emplace(Key, Raw);
            bLoadOrderDirty = true;
        }
    }

    void FPluginManager::ApplyProjectOverrides(const TVector<FProjectPluginOverride>& Overrides)
    {
        for (const auto& Override : Overrides)
        {
            FPlugin* Plugin = FindPlugin(Override.Name);
            if (!Plugin)
            {
                LOG_WARN("[PluginManager] .lproject lists plugin '{}' but no descriptor was found", Override.Name);
                continue;
            }
            if (Plugin->IsEnabled() != Override.bEnabled)
            {
                LOG_INFO("[PluginManager] Project overrides plugin '{}' to {}",
                    Override.Name, Override.bEnabled ? "Enabled" : "Disabled");
                Plugin->SetEnabled(Override.bEnabled);
                bLoadOrderDirty = true;
            }
        }
    }

    bool FPluginManager::IsModuleApplicable(const FPluginModuleDescriptor& Module) const
    {
        #if !WITH_EDITOR
            if (Module.Type == EPluginModuleType::Editor)
            {
                return false;
            }
        #endif

        #if defined(LUMINA_SHIPPING) || defined(LE_SHIPPING)
            if (Module.Type == EPluginModuleType::Developer)
            {
                return false;
            }
        #endif

        if (!Module.SupportedPlatforms.empty())
        {
            bool bFound = false;
            for (const FString& P : Module.SupportedPlatforms)
            {
                if (P == LUMINA_SYSTEM_NAME)
                {
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
            {
                return false;
            }
        }
        return true;
    }

    bool FPluginManager::IsPluginApplicable(const FPluginDescriptor& Desc) const
    {
        #if !WITH_EDITOR
            if (Desc.bEditorOnly)
            {
                return false;
            }
        #endif
        if (!Desc.SupportedPlatforms.empty())
        {
            bool bFound = false;
            for (const FString& P : Desc.SupportedPlatforms)
            {
                if (P == LUMINA_SYSTEM_NAME)
                {
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
            {
                return false;
            }
        }
        return true;
    }

    TVector<FPlugin*> FPluginManager::BuildLoadOrder()
    {
        // Kahn-style toposort; missing non-optional deps disable the dependent and log.
        // Iterate OwnedPlugins (insertion order), not PluginLookup, so order is reproducible.
        TVector<FPlugin*> Enabled;
        Enabled.reserve(OwnedPlugins.size());
        for (auto& Owned : OwnedPlugins)
        {
            FPlugin* P = Owned.get();
            if (!P->IsEnabled())                  continue;
            if (!IsPluginApplicable(P->GetDescriptor())) continue;

            bool bMissingRequired = false;
            for (const FPluginDependency& Dep : P->GetDescriptor().Dependencies)
            {
                FPlugin* DepPlugin = FindPlugin(Dep.Name);
                if (!DepPlugin || !DepPlugin->IsEnabled())
                {
                    if (!Dep.bOptional)
                    {
                        LOG_WARN("[PluginManager] Plugin '{}' requires '{}' which is missing or disabled; skipping",
                            P->GetName(), Dep.Name);
                        bMissingRequired = true;
                        break;
                    }
                }
            }
            if (bMissingRequired)
            {
                continue;
            }
            Enabled.push_back(P);
        }

        THashMap<FPlugin*, int32> InDegree;
        THashMap<FPlugin*, TVector<FPlugin*>> Edges;
        for (FPlugin* P : Enabled)
        {
            InDegree[P] = 0;
        }
        for (FPlugin* P : Enabled)
        {
            for (const FPluginDependency& Dep : P->GetDescriptor().Dependencies)
            {
                FPlugin* DepPlugin = FindPlugin(Dep.Name);
                if (!DepPlugin || !DepPlugin->IsEnabled())
                {
                    continue;
                }
                Edges[DepPlugin].push_back(P);
                InDegree[P] += 1;
            }
        }

        TVector<FPlugin*> Result;
        Result.reserve(Enabled.size());

        // Process the frontier as a FIFO and seed it in Enabled order so
        // peers (zero-indegree siblings) come out in discovery order.
        TVector<FPlugin*> Frontier;
        Frontier.reserve(Enabled.size());
        for (FPlugin* P : Enabled)
        {
            if (InDegree[P] == 0)
            {
                Frontier.push_back(P);
            }
        }
        size_t Head = 0;
        while (Head < Frontier.size())
        {
            FPlugin* P = Frontier[Head++];
            Result.push_back(P);
            // Edges were built in Enabled order, so iterating push_back
            // order keeps dependents stable too.
            for (FPlugin* Dependent : Edges[P])
            {
                if (--InDegree[Dependent] == 0)
                {
                    Frontier.push_back(Dependent);
                }
            }
        }

        if (Result.size() != Enabled.size())
        {
            // Cycle: emit leftovers in OwnedPlugins order (deterministic) and log each.
            THashSet<FPlugin*> Emitted;
            Emitted.reserve(Result.size());
            for (FPlugin* Q : Result) Emitted.insert(Q);

            FString Names;
            for (FPlugin* P : Enabled)
            {
                if (Emitted.find(P) == Emitted.end())
                {
                    if (!Names.empty()) Names += ", ";
                    Names.append(P->GetName().data(), P->GetName().size());
                    Result.push_back(P);
                }
            }
            LOG_WARN("[PluginManager] Dependency cycle detected involving: {}, loading them in discovery order; resolve the cycle to silence this.",
                Names);
        }
        return Result;
    }

    void FPluginManager::MountPluginContent(FPlugin& Plugin)
    {
        if (!Plugin.GetDescriptor().bContainsContent)
        {
            return;
        }
        if (Plugin.IsContentMounted())
        {
            return;
        }

        FString ContentDir = Plugin.GetContentDirectory();
        std::error_code EC;
        std::filesystem::path P(ContentDir.c_str());
        const bool bExists = std::filesystem::exists(P, EC);
        if (EC)
        {
            LOG_WARN("[PluginManager] exists({}) failed: {}", ContentDir, EC.message().c_str());
            return;
        }
        if (!bExists)
        {
            return;
        }
        EC.clear();
        if (!std::filesystem::is_directory(P, EC) || EC)
        {
            if (EC) LOG_WARN("[PluginManager] is_directory({}) failed: {}", ContentDir, EC.message().c_str());
            return;
        }

        FString Alias = Plugin.GetMountAlias();
        if (VFS::DoesAliasExists(FName(Alias)))
        {
            LOG_WARN("[PluginManager] VFS alias '{}' already exists; refusing to remount for plugin '{}'",
                Alias, Plugin.GetName());
            return;
        }

        VFS::Mount<VFS::FNativeFileSystem>(FFixedString(Alias.c_str()), ContentDir);
        Plugin.SetContentMounted(true);

        LOG_INFO("[PluginManager] Mounted plugin content {} -> {}", Alias, ContentDir);
    }

    bool FPluginManager::LoadPluginModule(FPlugin& Plugin, const FPluginModuleDescriptor& Module)
    {
        // Skip if already loaded (different phase pass, restart, etc.).
        for (const FLoadedPluginModule& Existing : Plugin.GetLoadedModules())
        {
            if (Existing.Descriptor.Name == Module.Name && Existing.bStartupCalled)
            {
                return true;
            }
        }

        // Monolithic builds statically link plugin modules (no DLL on disk); LoadModule's
        // static-factory branch resolves by bare name, so skip the DLL-existence pre-check.
        const FName BareName(Module.Name);
        if (FModuleManager::Get().HasStaticFactory(BareName))
        {
            IModuleInterface* StaticIface = FModuleManager::Get().LoadModule(Module.Name);
            if (!StaticIface)
            {
                LOG_WARN("[PluginManager] Plugin '{}' static module '{}' factory returned null",
                    Plugin.GetName(), Module.Name);
                return false;
            }

            FLoadedPluginModule Loaded;
            Loaded.Descriptor      = Module;
            Loaded.ModuleInterface = StaticIface;
            Loaded.bStartupCalled  = true;
            Plugin.GetLoadedModules().emplace_back(Move(Loaded));

            // DISPLAY, boot milestone; we want it visible in Shipping
            // post-mortems so "plugin X didn't load" is debuggable.
            LOG_DISPLAY("[PluginManager] Linked plugin '{}' static module '{}' (phase {})",
                Plugin.GetName(), Module.Name, LexToString(Module.LoadingPhase));
            return true;
        }

        FString DLLPath = Plugin.ResolveModuleBinaryPath(Module.Name);
        if (!Paths::Exists(DLLPath))
        {
            LOG_WARN("[PluginManager] Plugin '{}' module '{}' DLL missing at {}; skipping",
                Plugin.GetName(), Module.Name, DLLPath);
            return false;
        }

        IModuleInterface* Interface = FModuleManager::Get().LoadModule(DLLPath);
        if (!Interface)
        {
            LOG_WARN("[PluginManager] Failed to load plugin '{}' module '{}' at {}",
                Plugin.GetName(), Module.Name, DLLPath);
            return false;
        }

        FLoadedPluginModule Loaded;
        Loaded.Descriptor      = Module;
        Loaded.ModuleInterface = Interface;
        Loaded.bStartupCalled  = true; // FModuleManager called StartupModule
        Plugin.GetLoadedModules().emplace_back(Move(Loaded));

        // LOG_DISPLAY (not INFO) so this boot milestone survives Shipping.
        LOG_DISPLAY("[PluginManager] Loaded plugin '{}' module '{}' (phase {})",
            Plugin.GetName(), Module.Name, LexToString(Module.LoadingPhase));
        return true;
    }

    void FPluginManager::LoadModulesForPhase(EPluginLoadingPhase Phase)
    {
        if (bLoadOrderDirty)
        {
            CachedLoadOrder = BuildLoadOrder();
            bLoadOrderDirty = false;
        }

        // Mount content here, not in DiscoverDirectory (VFS isn't up at Earliest);
        // Earliest-phase modules see no content, which matches their contract.
        for (FPlugin* Plugin : CachedLoadOrder)
        {
            if (Phase != EPluginLoadingPhase::Earliest)
            {
                MountPluginContent(*Plugin);
            }

            for (const FPluginModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
            {
                if (Module.LoadingPhase != Phase)               continue;
                if (!IsModuleApplicable(Module))                continue;
                LoadPluginModule(*Plugin, Module);
            }
        }
    }

    void FPluginManager::ShutdownAllPlugins()
    {
        // Reverse-order teardown: walk CachedLoadOrder back-to-front so dependents shut
        // down before deps, calling UnloadModule for each so ShutdownModule runs in order.
        for (auto It = CachedLoadOrder.rbegin(); It != CachedLoadOrder.rend(); ++It)
        {
            FPlugin* Plugin = *It;
            auto& Loaded = Plugin->GetLoadedModules();
            for (auto MIt = Loaded.rbegin(); MIt != Loaded.rend(); ++MIt)
            {
                if (MIt->bStartupCalled)
                {
                    FModuleManager::Get().UnloadModule(FStringView(MIt->Descriptor.Name.c_str(), MIt->Descriptor.Name.size()));
                }
                MIt->ModuleInterface = nullptr;
            }
            Loaded.clear();
        }
        CachedLoadOrder.clear();
        bLoadOrderDirty = true;
    }

    FPlugin* FPluginManager::FindPlugin(FStringView Name)
    {
        auto It = PluginLookup.find(FName(Name));
        return It == PluginLookup.end() ? nullptr : It->second;
    }

    const FPlugin* FPluginManager::FindPlugin(FStringView Name) const
    {
        auto It = PluginLookup.find(FName(Name));
        return It == PluginLookup.end() ? nullptr : It->second;
    }

    TVector<FPlugin*> FPluginManager::GetEnabledPlugins()
    {
        TVector<FPlugin*> Result;
        Result.reserve(OwnedPlugins.size());
        for (auto& Owned : OwnedPlugins)
        {
            if (Owned->IsEnabled())
            {
                Result.push_back(Owned.get());
            }
        }
        return Result;
    }

    TVector<const FPlugin*> FPluginManager::GetAllPlugins() const
    {
        TVector<const FPlugin*> Result;
        Result.reserve(OwnedPlugins.size());
        for (const auto& Owned : OwnedPlugins)
        {
            Result.push_back(Owned.get());
        }
        return Result;
    }
}
