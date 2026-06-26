#pragma once
#include "Containers/Array.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    class CMaterial;
    class CMaterialNodeGraph;

    struct FMaterialGraphCompileResult
    {
        bool                            bSuccess = false;
        TVector<EdNodeGraph::FError>    Errors;
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
}
