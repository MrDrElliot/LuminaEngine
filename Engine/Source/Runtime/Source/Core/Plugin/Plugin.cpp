#include "RuntimePCH.h"
#include "Plugin.h"
#include "Paths/Paths.h"

namespace Lumina
{
    FPlugin::FPlugin(FPluginDescriptor InDescriptor, FString InPluginDirectory, FString InDescriptorPath)
        : Descriptor(Move(InDescriptor))
        , PluginDirectory(Move(InPluginDirectory))
        , DescriptorPath(Move(InDescriptorPath))
        , bEnabled(Descriptor.bEnabledByDefault)
    {
    }

    FString FPlugin::GetMountAlias() const
    {
        FString Result = "/";
        Result.append(Descriptor.Name.c_str(), Descriptor.Name.size());
        return Result;
    }

    FString FPlugin::GetContentDirectory() const
    {
        FString Result = PluginDirectory;
        Result += "/Content";
        return Result;
    }

    FString FPlugin::ResolveModuleBinaryPath(FStringView ModuleName) const
    {
        // Layout: <plugin>/Binaries/<Platform>/<Module>-<Cfg>.dll. PLATFORM_NAME keys the folder
        // (has arch); SYSTEM_NAME matches SupportedPlatforms (no arch), distinct, don't unify.
        FString Result = PluginDirectory;
        Result += "/Binaries/";
        Result += LUMINA_PLATFORM_NAME;
        Result += "/";
        Result += Paths::MakeModuleFileName(ModuleName).c_str();
        return Result;
    }
}
