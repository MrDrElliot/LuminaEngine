#pragma once

#include "MaterialNodeGraph.h"
#include "MaterialFunctionGraph.generated.h"

namespace Lumina
{
    class CMaterialFunction;

    // Name of the function graph child object inside a CMaterialFunction's package. Shared by the
    // function editor (which creates/loads it) and the call node's inliner (which loads it).
    inline constexpr const char* GMaterialFunctionGraphObjectName = "MaterialFunctionGraph";

    // The editable graph of a material function: the material node library minus the output node, plus
    // FunctionInput / FunctionOutput. Produces no shader itself; the call node inlines it into a host material.
    REFLECT()
    class CMaterialFunctionGraph : public CMaterialNodeGraph
    {
        GENERATED_BODY()

    public:

        // Runs each node's GenerateDefinition into a throwaway compiler to surface type errors. Walks
        // from every FunctionOutput node; FunctionInput nodes emit their preview-default constants.
        void CompileForValidation(FMaterialCompiler& Compiler);

    protected:

        // Function graphs have no material output node; the author adds FunctionInput / FunctionOutput.
        void EnsureRootNodes() override {}

        // A function graph has one output node per declared output, so every one of them is a root.
        bool IsGraphRootNode(CEdGraphNode* Node) const override;
    };
}
