#pragma once

#include "MaterialNodeGraph.h"
#include "MaterialFunctionGraph.generated.h"

namespace Lumina
{
    class CMaterialFunction;

    // Child object name shared by the function editor and the call node's inliner.
    inline constexpr const char* GMaterialFunctionGraphObjectName = "MaterialFunctionGraph";

    // The material node library minus the output node, plus FunctionInput and FunctionOutput, inlined by the call node.
    REFLECT()
    class CMaterialFunctionGraph : public CMaterialNodeGraph
    {
        GENERATED_BODY()

    public:

        // Walks from every FunctionOutput into a throwaway compiler, with FunctionInput nodes emitting preview-default constants.
        void CompileForValidation(FMaterialCompiler& Compiler);

    protected:

        // Function graphs have no material output node; the author adds FunctionInput / FunctionOutput.
        void EnsureRootNodes() override {}

        // A function graph has one output node per declared output, so every one of them is a root.
        bool IsGraphRootNode(CEdGraphNode* Node) const override;
    };
}
