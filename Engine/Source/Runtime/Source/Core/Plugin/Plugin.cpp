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
        Result.Append(*Descriptor.Name, Descriptor.Name.Len());
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
        // PLATFORM_NAME keys the folder and carries arch, while SYSTEM_NAME matches SupportedPlatforms.
        FString Result = PluginDirectory;
        Result += "/Binaries/";
        Result += LUMINA_PLATFORM_NAME;
        Result += "/";
        Result += *Paths::MakeModuleFileName(ModuleName);
        return Result;
    }
}
