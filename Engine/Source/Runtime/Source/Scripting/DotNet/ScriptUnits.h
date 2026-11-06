#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Scripting/DotNet/ScriptReferences.h"

// Which assemblies the engine compiles from which sources, and the IDE projects that mirror them.
namespace Lumina::DotNet
{
    // Keeps each source file's text alive while it is marshaled to managed.
    struct FGatheredSource
    {
        FString Path;
        FString Text;
    };

    // One assembly the engine compiles, the single description both the compile and the IDE projects are built from.
    struct FScriptUnit
    {
        FString          Name;            // assembly label, which becomes the managed assembly name
        FString          DiskDir;         // absolute Scripts dir on disk, the source root and .csproj location
        FString          BinaryDir;       // <root>/Binaries/DotNet, where this unit's compiled DLL is emitted
        FString          IntermediateDir; // <root>/Intermediates/DotNet/<Name>, the IDE obj dir
        FString          AssemblyPath;    // <BinaryDir>/<Name>.dll, the emit target with sources and the load source without
        TVector<FString> Deps;            // sibling unit names this one references
        TVector<FString> References;      // absolute paths to third-party assemblies, packages and declared DLLs
        FScriptReferenceSet Declared;     // kept so the generated project can name the packages rather than their DLLs
    };

    FString Join(FStringView Left, FStringView Right);

    // A path the OS accepts, with native separators.
    FString NativePath(FStringView Path);

    void GatherSourcesUnder(const FString& DiskDir, TVector<FGatheredSource>& Out);

    // Tells a stale emitted artifact apart from an authored prebuilt assembly at the same path.
    FString CompiledMarkerPath(const FString& DllPath);

    TVector<FScriptUnit> BuildScriptUnits();
    TVector<FScriptUnit> BuildCookedScriptUnits();
    void WriteScriptSolution(const TVector<FScriptUnit>& Units);
}
