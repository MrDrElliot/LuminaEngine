#pragma once

#include "PluginLoadingPhase.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    enum class EPluginModuleType : uint8
    {
        // Loaded in standalone game and editor.
        Runtime,

        // Loaded only when WITH_EDITOR. Stripped from shipping builds.
        Editor,

        // Loaded in Debug and Development only, for tooling that should not ship.
        Developer,

        // A Reflector generator, which the engine never loads.
        Reflection,
    };

    RUNTIME_API FStringView LexToString(EPluginModuleType Type);

    // False for a name that is not a module type; the descriptor loader reports it, as the build tool does.
    RUNTIME_API bool TryParsePluginModuleType(FStringView Str, EPluginModuleType& OutType);

    struct FPluginModuleDescriptor
    {
        FString             Name;
        EPluginModuleType   Type            = EPluginModuleType::Runtime;
        EPluginLoadingPhase LoadingPhase    = EPluginLoadingPhase::PreEngineInit;

        // Optional platform allow-list. Empty == all platforms.
        TVector<FString>    SupportedPlatforms;
    };

    // Written as a bare plugin name, or as { "Name", "Optional", "Version" }; LuminaBuildTool reads the same two forms.
    struct FPluginDependency
    {
        FString Name;

        // The lowest Version of the dependency that satisfies this; 0 accepts any.
        int32   Version  = 0;

        // A missing or too-old optional dependency is no error; it only stops ordering against it.
        bool    bOptional = false;
    };

    // Parsed contents of a .lplugin file, while FPlugin owns the live runtime state.
    struct FPluginDescriptor
    {
        int32   FormatVersion       = 1;
        FString Name;
        int32   Version             = 1;
        FString VersionName;
        FString Author;
        FString Description;
        FString Category;

        bool    bEnabledByDefault   = true;
        bool    bEditorOnly         = false;
        bool    bContainsContent    = true;

        // Set by discovery from which root the descriptor was found under.
        bool    bIsEnginePlugin     = true;

        TVector<FString>                 SupportedPlatforms;
        TVector<FPluginDependency>       Dependencies;
        TVector<FPluginModuleDescriptor> Modules;

        // Cook-root seeds, and when empty the plugin's assets cook only if reached from other roots.
        TVector<FCookRoot>               CookRoots;

        // False with OutError set for a missing file, malformed JSON, a missing Name, or a field LuminaBuildTool would also reject.
        RUNTIME_API static bool LoadFromFile(FStringView FilePath, FPluginDescriptor& OutDescriptor, FString& OutError);

        // Parse a .lplugin from an in-memory buffer.
        RUNTIME_API static bool LoadFromString(FStringView Json, FPluginDescriptor& OutDescriptor, FString& OutError);
    };
}
