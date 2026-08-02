#pragma once

#include "PluginLoadingPhase.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Containers/Array.h"
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

        // Loaded in Debug/Development only. Useful for tooling that should
        // not ship.
        Developer,
    };

    RUNTIME_API FStringView LexToString(EPluginModuleType Type);
    RUNTIME_API EPluginModuleType ParsePluginModuleType(FStringView Str, EPluginModuleType Default = EPluginModuleType::Runtime);

    struct FPluginModuleDescriptor
    {
        FString             Name;
        EPluginModuleType   Type            = EPluginModuleType::Runtime;
        EPluginLoadingPhase LoadingPhase    = EPluginLoadingPhase::PreEngineInit;

        // Optional platform allow-list. Empty == all platforms.
        TVector<FString>    SupportedPlatforms;
    };

    struct FPluginDependency
    {
        FString Name;
        int32   Version  = 0;       // 0 == any
        bool    bOptional = false;  // missing optional dep is not an error
    };

    // Parsed contents of a .lplugin descriptor file. Not the live runtime
    // state; FPlugin owns that.
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

        // Set by the discovery layer based on which root the descriptor was
        // found under (Engine/Plugins/* vs <Project>/Plugins/*).
        bool    bIsEnginePlugin     = true;

        TVector<FString>                 SupportedPlatforms;
        TVector<FPluginDependency>       Dependencies;
        TVector<FPluginModuleDescriptor> Modules;

        // Cook-root seeds contributed by this plugin; empty = assets cook only if
        // reached transitively from project/engine roots.
        TVector<FCookRoot>               CookRoots;

        // Load + parse a .lplugin file. Returns false + populates OutError on
        // any failure (missing file, malformed JSON, missing required field).
        RUNTIME_API static bool LoadFromFile(FStringView FilePath, FPluginDescriptor& OutDescriptor, FString& OutError);

        // Parse a .lplugin from an in-memory buffer.
        RUNTIME_API static bool LoadFromString(FStringView Json, FPluginDescriptor& OutDescriptor, FString& OutError);
    };
}
