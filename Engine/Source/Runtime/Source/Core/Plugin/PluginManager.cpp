#include "RuntimePCH.h"
#include "PluginManager.h"
#include "Plugin.h"
#include "PluginDescriptor.h"

#include "Core/Module/ModuleManager.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/NativeFileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"

#include "Platform/Filesystem/PlatformFilesystem.h"

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
        if (EngineDir.IsEmpty())
        {
            return;
        }
        FString Root = EngineDir;
        Root += "/Plugins";
        DiscoverDirectory(Root, /*bIsEngine*/true);
    }

    void FPluginManager::DiscoverProjectPlugins(FStringView ProjectDir)
    {
        if (ProjectDir.IsEmpty() || FStringView(ProjectDirectory) == ProjectDir)
        {
            return;
        }
        ProjectDirectory = FStringView(ProjectDir.GetData(), ProjectDir.Len());
        FString Root = ProjectDirectory;
        Root += "/Plugins";
        DiscoverDirectory(Root, /*bIsEngine*/false);
    }

    void FPluginManager::DiscoverDirectory(FStringView RootView, bool bIsEngine)
    {
        if (!Filesystem::IsDirectory(RootView))
        {
            return;
        }

        // Each subdir is a candidate plugin; prefer <SubDir>/<SubDir>.lplugin, else any .lplugin.
        Filesystem::IterateDirectory(RootView, [this, bIsEngine](const Filesystem::FDirectoryEntry& Entry)
        {
            if (!Entry.IsDirectory())
            {
                return;
            }

            const FString PluginDir(Entry.FullPath.GetData(), Entry.FullPath.Len());

            FString Conventional = PluginDir;
            Conventional.AppendChar('/');
            Conventional.Append(Entry.Name.GetData(), Entry.Name.Len());
            Conventional.Append(".lplugin");

            FString Descriptor;

            if (Filesystem::Exists(Conventional))
            {
                Descriptor = Conventional;
            }
            else
            {
                Filesystem::IterateDirectory(PluginDir, [&](const Filesystem::FDirectoryEntry& Candidate) -> Filesystem::EVisit
                {
                    if (Candidate.IsDirectory() || Candidate.GetExtension() != FStringView(".lplugin"))
                    {
                        return Filesystem::EVisit::Continue;
                    }

                    Descriptor = FStringView(Candidate.FullPath.GetData(), Candidate.FullPath.Len());

                    // Warn so a misnamed descriptor surfaces instead of "first .lplugin wins" silently.
                    LOG_WARN("[PluginManager] {} contains a .lplugin not matching the folder name; using {} (rename to {})",
                        PluginDir, Candidate.Name, Conventional);

                    return Filesystem::EVisit::Stop;
                });
            }

            if (Descriptor.IsEmpty())
            {
                return;
            }

            FString DescriptorPath = Descriptor;
            FString PluginDirStr   = PluginDir;
            Paths::Normalize(DescriptorPath);
            Paths::Normalize(PluginDirStr);

            FPluginDescriptor Parsed;
            FString Error;
            if (!FPluginDescriptor::LoadFromFile(DescriptorPath, Parsed, Error))
            {
                LOG_WARN("[PluginManager] Skipping plugin at {}: {}", DescriptorPath, Error);
                return;
            }
            Parsed.bIsEnginePlugin = bIsEngine;

            // On a duplicate name the first wins and the later ones are logged.
            FName Key(Parsed.Name);
            if (PluginLookup.Contains(Key))
            {
                LOG_WARN("[PluginManager] Duplicate plugin name '{}' at {} ignored (already registered)",
                    Parsed.Name, DescriptorPath);
                return;
            }

            LOG_INFO("[PluginManager] Discovered {} plugin '{}' ({} modules)",
                bIsEngine ? "engine" : "project", Parsed.Name, Parsed.Modules.Num());

            // Nothing else puts a plugin's Binaries directory on the search path for a dependent DLL.
            FString BinariesDir = PluginDirStr;
            BinariesDir += "/Binaries/";
            BinariesDir += LUMINA_PLATFORM_NAME;

            if (Paths::Exists(BinariesDir))
            {
                Platform::AddDLLDirectory(BinariesDir);
            }

            TUniquePtr<FPlugin> Owned = MakeUnique<FPlugin>(Move(Parsed), Move(PluginDirStr), Move(DescriptorPath));
            FPlugin* Raw = Owned.get();
            OwnedPlugins.Emplace(Move(Owned));
            PluginLookup.TryAdd(Key, Raw);
            bLoadOrderDirty = true;
        });
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

        if (!Module.SupportedPlatforms.IsEmpty())
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
        if (!Desc.SupportedPlatforms.IsEmpty())
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
        // Iterates OwnedPlugins in insertion order rather than the lookup, so the order is reproducible.
        TVector<FPlugin*> Enabled;
        Enabled.Reserve(OwnedPlugins.Num());
        THashSet<FPlugin*> Loadable;
        for (auto& Owned : OwnedPlugins)
        {
            FPlugin* P = Owned.get();
            if (P->IsEnabled() && IsPluginApplicable(P->GetDescriptor()))
            {
                Enabled.Add(P);
                Loadable.Add(P);
            }
        }

        // The dependency that is loading, at the version asked for, or null; so a skipped plugin also fails its dependents.
        const auto Resolve = [this, &Loadable](const FPluginDependency& Dep) -> FPlugin*
        {
            FPlugin* DepPlugin = FindPlugin(Dep.Name);
            return DepPlugin != nullptr && Loadable.Contains(DepPlugin) && DepPlugin->GetDescriptor().Version >= Dep.Version
                ? DepPlugin : nullptr;
        };

        // Repeated until stable, so skipping one plugin also skips whatever required it; LuminaBuildTool prunes the same way.
        for (bool bChanged = true; bChanged;)
        {
            bChanged = false;
            Enabled.RemoveAll([&](FPlugin* P)
            {
                for (const FPluginDependency& Dep : P->GetDescriptor().Dependencies)
                {
                    if (Dep.bOptional || Resolve(Dep) != nullptr)
                    {
                        continue;
                    }
                    FPlugin* Found = FindPlugin(Dep.Name);
                    if (Found != nullptr && Loadable.Contains(Found))
                    {
                        LOG_WARN("[PluginManager] Plugin '{}' requires '{}' version {} but found version {}; skipping",
                            P->GetName(), Dep.Name, Dep.Version, Found->GetDescriptor().Version);
                    }
                    else
                    {
                        LOG_WARN("[PluginManager] Plugin '{}' requires '{}' which is missing, disabled or not loading; skipping",
                            P->GetName(), Dep.Name);
                    }
                    Loadable.Remove(P);
                    bChanged = true;
                    return true;
                }
                return false;
            });
        }

        THashMap<FPlugin*, int32> InDegree;
        THashMap<FPlugin*, TVector<FPlugin*>> Edges;
        for (FPlugin* P : Enabled)
        {
            InDegree.Add(P, 0);
        }
        for (FPlugin* P : Enabled)
        {
            for (const FPluginDependency& Dep : P->GetDescriptor().Dependencies)
            {
                if (FPlugin* DepPlugin = Resolve(Dep))
                {
                    Edges.FindOrAdd(DepPlugin).Add(P);
                    InDegree.FindChecked(P) += 1;
                }
            }
        }

        TVector<FPlugin*> Result;
        Result.Reserve(Enabled.Num());

        // Seeded in Enabled order so zero-indegree siblings come out in discovery order.
        TVector<FPlugin*> Frontier;
        Frontier.Reserve(Enabled.Num());
        for (FPlugin* P : Enabled)
        {
            if (InDegree.FindChecked(P) == 0)
            {
                Frontier.Add(P);
            }
        }
        int32 Head = 0;
        while (Head < Frontier.Num())
        {
            FPlugin* P = Frontier[Head++];
            Result.Add(P);
            // Edges were built in Enabled order, so iterating push order keeps dependents stable.
            if (const TVector<FPlugin*>* Dependents = Edges.Find(P))
            {
                for (FPlugin* Dependent : *Dependents)
                {
                    if (--InDegree.FindChecked(Dependent) == 0)
                    {
                        Frontier.Add(Dependent);
                    }
                }
            }
        }

        if (Result.Num() != Enabled.Num())
        {
            // On a cycle, leftovers are emitted in OwnedPlugins order and each is logged.
            THashSet<FPlugin*> Emitted;
            Emitted.Reserve(Result.Num());
            for (FPlugin* Q : Result) Emitted.Add(Q);

            FString Names;
            for (FPlugin* P : Enabled)
            {
                if (!Emitted.Contains(P))
                {
                    if (!Names.IsEmpty()) Names += ", ";
                    Names.Append(P->GetName().GetData(), P->GetName().Len());
                    Result.Add(P);
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
        if (!Filesystem::IsDirectory(ContentDir))
        {
            return;
        }

        FString Alias = Plugin.GetMountAlias();
        if (VFS::DoesAliasExists(FName(Alias)))
        {
            LOG_WARN("[PluginManager] VFS alias '{}' already exists; refusing to remount for plugin '{}'",
                Alias, Plugin.GetName());
            return;
        }

        VFS::Mount<VFS::FNativeFileSystem>(FFixedString(*Alias), ContentDir);
        Plugin.SetContentMounted(true);

        LOG_INFO("[PluginManager] Mounted plugin content {} -> {}", Alias, ContentDir);
    }

    bool FPluginManager::LoadPluginModule(FPlugin& Plugin, const FPluginModuleDescriptor& Module)
    {
        // Skip if already loaded (different phase pass, restart, etc.).
        const bool bAlreadyLoaded = Algo::AnyOf(Plugin.GetLoadedModules(),
            [&Module](const FLoadedPluginModule& Existing)
            {
                return Existing.Descriptor.Name == Module.Name && Existing.bStartupCalled;
            });

        if (bAlreadyLoaded)
        {
            return true;
        }

        // A monolithic build has no DLL on disk, and the static factory resolves by bare name.
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
            Plugin.GetLoadedModules().Emplace(Move(Loaded));

            // A boot milestone worth seeing in Shipping, so a plugin failing to load stays debuggable.
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
        Plugin.GetLoadedModules().Emplace(Move(Loaded));

        // LOG_DISPLAY (not INFO) so this boot milestone survives Shipping.
        LOG_DISPLAY("[PluginManager] Loaded plugin '{}' module '{}' (phase {})",
            Plugin.GetName(), Module.Name, LexToString(Module.LoadingPhase));
        return true;
    }

    void FPluginManager::LoadModulesForPhase(EPluginLoadingPhase Phase)
    {
        PhaseFired[(int32)Phase] = true;

        if (bLoadOrderDirty)
        {
            CachedLoadOrder = BuildLoadOrder();
            bLoadOrderDirty = false;
        }

        // The VFS is not up at the earliest phase, and those modules see no content by contract.
        for (FPlugin* Plugin : CachedLoadOrder)
        {
            if (Phase != EPluginLoadingPhase::Earliest)
            {
                MountPluginContent(*Plugin);
            }

            for (const FPluginModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
            {
                // A plugin discovered after its module's phase fired catches up here, but only once.
                const bool bLate = Module.LoadingPhase != Phase && PhaseFired[(int32)Module.LoadingPhase];
                if (Module.LoadingPhase != Phase && !bLate)     continue;
                if (!IsModuleApplicable(Module))                continue;
                if (bLate && !AttemptedModules.TryAdd(FName(Module.Name)))
                {
                    continue;
                }
                AttemptedModules.Emplace(Module.Name);
                const bool bLoaded = Algo::AnyOf(Plugin->GetLoadedModules(),
                    [&Module](const FLoadedPluginModule& Existing) { return Existing.Descriptor.Name == Module.Name && Existing.bStartupCalled; });
                if (bLate && !bLoaded)
                {
                    LOG_INFO("[PluginManager] Plugin '{}' module '{}' asked for phase {}, which had already run; loading it at {}.",
                        Plugin->GetName(), Module.Name, LexToString(Module.LoadingPhase), LexToString(Phase));
                }
                LoadPluginModule(*Plugin, Module);
            }
        }
    }

    void FPluginManager::ShutdownAllPlugins()
    {
        // Walks back to front so dependents shut down before the plugins they depend on.
        for (auto It = CachedLoadOrder.rbegin(); It != CachedLoadOrder.rend(); ++It)
        {
            FPlugin* Plugin = *It;
            auto& Loaded = Plugin->GetLoadedModules();
            for (auto MIt = Loaded.rbegin(); MIt != Loaded.rend(); ++MIt)
            {
                if (MIt->bStartupCalled)
                {
                    FModuleManager::Get().UnloadModule(FStringView(*MIt->Descriptor.Name, MIt->Descriptor.Name.Len()));
                }
                MIt->ModuleInterface = nullptr;
            }
            Loaded.Reset();
        }
        CachedLoadOrder.Reset();
        bLoadOrderDirty = true;
    }

    FPlugin* FPluginManager::FindPlugin(FStringView Name)
    {
        auto It = PluginLookup.Find(FName(Name));
        return It == nullptr ? nullptr : (*It);
    }

    const FPlugin* FPluginManager::FindPlugin(FStringView Name) const
    {
        auto It = PluginLookup.Find(FName(Name));
        return It == nullptr ? nullptr : (*It);
    }

    TVector<FPlugin*> FPluginManager::GetEnabledPlugins()
    {
        TVector<FPlugin*> Result;
        Result.Reserve(OwnedPlugins.Num());
        for (auto& Owned : OwnedPlugins)
        {
            if (Owned->IsEnabled())
            {
                Result.Add(Owned.get());
            }
        }
        return Result;
    }

    TVector<const FPlugin*> FPluginManager::GetAllPlugins() const
    {
        TVector<const FPlugin*> Result;
        Result.Reserve(OwnedPlugins.Num());
        for (const auto& Owned : OwnedPlugins)
        {
            Result.Add(Owned.get());
        }
        return Result;
    }
}
