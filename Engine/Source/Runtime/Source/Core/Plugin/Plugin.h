#pragma once

#include "PluginDescriptor.h"
#include "PluginLoadingPhase.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class IModuleInterface;

    // Runtime state for one loaded plugin module; scoped to the plugin for ordered unload.
    struct FLoadedPluginModule
    {
        FPluginModuleDescriptor Descriptor;
        IModuleInterface*       ModuleInterface = nullptr;
        bool                    bStartupCalled  = false;
    };

    // Runtime state for one plugin: parsed descriptor, loaded modules, and discovery
    // location (resolves module DLL paths + Content/).
    class RUNTIME_API FPlugin
    {
    public:
        FPlugin(FPluginDescriptor InDescriptor, FString InPluginDirectory, FString InDescriptorPath);

        const FPluginDescriptor& GetDescriptor()    const { return Descriptor; }
        FStringView              GetName()          const { return Descriptor.Name; }
        FStringView              GetDirectory()     const { return PluginDirectory; }
        FStringView              GetDescriptorPath() const { return DescriptorPath; }
        bool                     IsEnginePlugin()   const { return Descriptor.bIsEnginePlugin; }

        bool                     IsEnabled()        const { return bEnabled; }
        void                     SetEnabled(bool b)       { bEnabled = b; }

        bool                     IsContentMounted() const { return bContentMounted; }
        void                     SetContentMounted(bool b){ bContentMounted = b; }

        TVector<FLoadedPluginModule>&       GetLoadedModules()       { return LoadedModules; }
        const TVector<FLoadedPluginModule>& GetLoadedModules() const { return LoadedModules; }

        // Mount alias for VFS: "/" + Name.
        FString GetMountAlias() const;

        // Absolute path to the plugin's content directory ("<dir>/Content").
        FString GetContentDirectory() const;

        // Absolute path to where a module's DLL is expected to live.
        FString ResolveModuleBinaryPath(FStringView ModuleName) const;

    private:
        FPluginDescriptor            Descriptor;
        FString                      PluginDirectory;   // absolute, no trailing slash
        FString                      DescriptorPath;    // absolute .lplugin path

        bool                         bEnabled         = true;
        bool                         bContentMounted  = false;
        TVector<FLoadedPluginModule> LoadedModules;
    };
}
