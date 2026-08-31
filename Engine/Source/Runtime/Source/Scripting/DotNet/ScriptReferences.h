#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::DotNet
{
    struct FScriptPackageRef
    {
        FString Name;
        FString Version;
    };

    // What a .lplugin or .lproject declares under "ScriptReferences".
    struct FScriptReferenceSet
    {
        TVector<FScriptPackageRef> Packages;
        TVector<FString>           Assemblies;

        bool IsEmpty() const { return Packages.empty() && Assemblies.empty(); }
    };

    // Reads the "ScriptReferences" object from a descriptor. Assemblies resolve against RootDir when relative.
    RUNTIME_API bool ParseScriptReferences(FStringView DescriptorPath, FStringView RootDir, FScriptReferenceSet& Out);

    // Restores declared packages into RestoreDir and returns every assembly the unit references, as absolute paths.
    RUNTIME_API bool ResolveScriptReferences(FStringView UnitName, FStringView RestoreDir,
        const FScriptReferenceSet& Declared, TVector<FString>& OutAssemblyPaths);
}
