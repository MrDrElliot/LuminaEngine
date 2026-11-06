#pragma once
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include <atomic>
#include "Renderer/ShaderCompiler.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    class CMaterial;
    class CMaterialNodeGraph;

    // Which shader set a compile run builds; default-constructed targets the material's own stages.
    struct FMaterialCompileTarget
    {
        // False writes the material's own stages and clears its permutations, whose bits it renumbers.
        bool bPermutation = false;

        // Switch values this permutation compiles with; a switch absent from it takes its graph default.
        THashMap<FName, bool> StaticSwitchOverrides;

        // The key those values resolve to against the material's current switch manifest.
        uint64 Key = 0;

        // Manifest the key was minted under, so a recompile mid-compile refuses the stale stages.
        uint32 Generation = 0;
    };

    // Stages a compile dispatched and stages the shader compiler handed back, since a failed stage leaves the last good bytecode in place.
    struct FMaterialStageCommitLog
    {
        std::atomic<uint32> Dispatched{0};
        std::atomic<uint32> Committed{0};
        FShaderCompileWaiter Stages;
    };

    struct FMaterialGraphCompileResult
    {
        bool                            bSuccess = false;
        TSharedPtr<FMaterialStageCommitLog> StageLog;
        TVector<EdNodeGraph::FError>    Errors;
        // Non-fatal findings; populated on SUCCESS too, which is the point of them.
        TVector<EdNodeGraph::FError>    Warnings;
        FString                         PixelSource;
        FString                         VertexSource;
        // What the material itself contributes to its shaders, which keys its artifact cache entries.
        uint64                          SourceHash = 0;
        FMaterialCompiler::FShaderStats Stats;
    };

    // Compiles Graph into Material and registers it for render; on a graph error returns bSuccess=false and leaves the material not-ready.
    EDITOR_API FMaterialGraphCompileResult CompileMaterialGraph(CMaterial* Material, CMaterialNodeGraph* Graph);

    // Finish once OutResult.StageLog settles, holding Compiler and a strong ref to Material until then, since stage callbacks write into both.
    EDITOR_API bool BeginMaterialGraphCompile(CMaterial* Material, CMaterialNodeGraph* Graph, FMaterialCompiler& Compiler,
                                              FMaterialGraphCompileResult& OutResult, const FMaterialCompileTarget& Target = {},
                                              Jobs::EJobPriority Priority = Jobs::EJobPriority::Normal);
    EDITOR_API void FinishMaterialGraphCompile(CMaterial* Material, FMaterialCompiler& Compiler,
                                               FMaterialGraphCompileResult& InOutResult, const FMaterialCompileTarget& Target = {});

    // Inverts Key back into switch values; false when the manifest can express no such combination.
    EDITOR_API bool MakeMaterialPermutationTarget(const CMaterial* Material, uint64 Key, FMaterialCompileTarget& OutTarget);

    // The graph saved beside the material, or null for a cooked or code-built material that has none.
    EDITOR_API CMaterialNodeGraph* LoadMaterialGraph(CMaterial* Material);

    EDITOR_API void QueueMaterialFunctionRecompiles(const FGuid& FunctionGUID);

    // Blocking recompile of a material saved against older shader templates, so a cook never ships stages the game cannot use.
    EDITOR_API bool RecompileMaterialIfStale(CMaterial* Material, FString& OutError);

    // Blocking. Pulls each permutation from the artifact cache or compiles it, returning how many could not be built.
    EDITOR_API uint32 EnsureMaterialPermutations(CMaterial* Material, TSpan<const uint64> Keys);
}
