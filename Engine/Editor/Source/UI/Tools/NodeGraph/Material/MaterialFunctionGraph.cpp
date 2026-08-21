#include "MaterialFunctionGraph.h"

#include "MaterialCompiler.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Nodes/MaterialNodes.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"

namespace Lumina
{
    bool CMaterialFunctionGraph::IsGraphRootNode(CEdGraphNode* Node) const
    {
        return Node != nullptr && Node->IsA<CMaterialFunctionOutput>();
    }

    void CMaterialFunctionGraph::CompileForValidation(FMaterialCompiler& Compiler)
    {
        if (Nodes.empty())
        {
            return;
        }

        for (CEdGraphNode* Node : Nodes)
        {
            Node->ClearError();
        }

        // Walks from every output node, so a branch feeding another output is still validated.
        TVector<CEdGraphNode*> SortedNodes;
        CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoots(Nodes, SortedNodes, [](CEdGraphNode* Node)
        {
            return Node->IsA<CMaterialFunctionOutput>();
        });

        if (CyclicNode != nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Cyclic";
            Error.Description  = "Cycle detected in material function graph! Graph must be acyclic!";
            Error.Node        = CyclicNode;
            Compiler.AddError(Error);
            return;
        }

        // Functions only ever contribute to the pixel stage during validation; WPO is a material concept.
        Compiler.SetStage(EMaterialCompileStage::Pixel);
        for (size_t i = 0; i < SortedNodes.size(); ++i)
        {
            CEdGraphNode* Node = SortedNodes[i];
            Node->SetDebugExecutionOrder((uint32)i);

            // Reroutes pass through, an input emits its preview default, and an output validates its type.
            if (Node->IsRerouteNode())
            {
                continue;
            }

            static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
        }

        for (auto& Error : Compiler.GetErrors())
        {
            if (Error.Node)
            {
                Error.Node->SetError(Error);
            }
        }
    }
}
