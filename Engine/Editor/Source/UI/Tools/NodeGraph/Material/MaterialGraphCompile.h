#pragma once
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
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

    struct FMaterialGraphCompileResult
    {
        bool                            bSuccess = false;
        TVector<EdNodeGraph::FError>    Errors;
        // Non-fatal findings; populated on SUCCESS too, which is the point of them.
        TVector<EdNodeGraph::FError>    Warnings;
        FString                         PixelSource;
        FString                         VertexSource;
        FMaterialCompiler::FShaderStats Stats;
    };

    // Compiles Graph into Material: builds every shader stage (PS/VS, plus MS/VisBuffer/Deferred for the PBR
    // domain), commits them to the shader library, fills Material->Textures/Parameters, then calls
    // Material->PostLoad() so it registers and is ready for render. On a graph error returns bSuccess=false
    // with the errors and leaves the material not-ready. Editor-only (drives GShaderCompiler). Shared by the
    // material editor tool and the scene importer's procedural material generation.
    EDITOR_API FMaterialGraphCompileResult CompileMaterialGraph(CMaterial* Material, CMaterialNodeGraph* Graph);

    // Blocking compile of one permutation, leaving the material's own stages untouched.
    EDITOR_API FMaterialGraphCompileResult CompileMaterialPermutation(CMaterial* Material, CMaterialNodeGraph* Graph, uint64 Key);

    // Non-blocking split of the above, for callers that must not sit on the shader task swarm.
    //
    // Begin runs the graph and DISPATCHES every stage compile, returning as soon as they are queued. False
    // means nothing was dispatched (null argument or a graph error) and OutResult already says why -- so a
    // caller that parks state on a true return can never park it on a compile that will never land. Once
    // GShaderCompiler->HasPendingRequests() reads false, Finish does the committing half: stage validation,
    // texture/parameter extraction, template-hash stamp, PostLoad.
    //
    // Two things must outlive the gap. Compiler, because Finish reads the bound textures and parameters
    // back off it. And Material, because the per-stage commit callbacks capture it RAW and fire on a
    // worker -- hold a strong ref for the whole wait.
    EDITOR_API bool BeginMaterialGraphCompile(CMaterial* Material, CMaterialNodeGraph* Graph, FMaterialCompiler& Compiler,
                                              FMaterialGraphCompileResult& OutResult, const FMaterialCompileTarget& Target = {});
    EDITOR_API void FinishMaterialGraphCompile(CMaterial* Material, FMaterialCompiler& Compiler,
                                               FMaterialGraphCompileResult& InOutResult, const FMaterialCompileTarget& Target = {});

    // Inverts Key back into switch values; false when the manifest can express no such combination.
    EDITOR_API bool MakeMaterialPermutationTarget(const CMaterial* Material, uint64 Key, FMaterialCompileTarget& OutTarget);

    // Editor-tick drain for CMaterial's stale-template queue (materials whose serialized shaders were
    // built against older templates, detected in PostLoad by CompiledTemplateHash mismatch). Call once per
    // frame (EditorUI::OnUpdate); no-op when the queue is empty and nothing is in flight.
    //
    // Recompiles at most ONE material at a time, via Begin/Finish above: the dispatch happens on one call
    // and the commit on a later one, so a ~600ms multi-stage compile costs the frame its dispatch rather
    // than the whole compile. Marks the package dirty and toasts on the finishing call.
    EDITOR_API void ProcessStaleMaterialRecompiles();

    // Editor-tick drain for CMaterial's permutation queue, one dispatch-then-poll compile at a time.
    EDITOR_API void ProcessMaterialPermutationRequests();
}
