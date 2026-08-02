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
        Result.append(ModuleName.data(), ModuleName.size());
        Result += "-";
        Result += LUMINA_CONFIGURATION_NAME;
        Result += LUMINA_SHAREDLIB_EXT_NAME;
        return Result;
    }
}
