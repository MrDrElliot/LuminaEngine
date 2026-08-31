#include "ParticleNodeGraph.h"
#include "ParticleCompiler.h"
#include "Core/Object/Cast.h"
#include "Nodes/ParticleOutputNode.h"
#include "Nodes/ParticleExpressionNodes.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"

namespace Lumina
{
    void CParticleNodeGraph::Initialize()
    {
        Super::Initialize();

        const bool bHasOutputNode = Algo::AnyOf(Nodes, [](const TObjectPtr<CEdGraphNode>& Node)
        {
            return Node.IsValid() && Node->IsA<CParticleOutputNode>();
        });

        if (!bHasOutputNode)
        {
            CreateNode(CParticleOutputNode::StaticClass());
        }

        ValidateGraph();
    }

    void CParticleNodeGraph::Shutdown()
    {
        CEdNodeGraph::Shutdown();
    }

    void CParticleNodeGraph::CompileGraph(FParticleCompiler& Compiler)
    {
        if (Nodes.empty())
        {
            return;
        }

        for (CEdGraphNode* Node : Nodes)
        {
            Node->ClearError();
        }

        // Cycle detection only, since emission is demand-driven from the output node.
        TVector<CEdGraphNode*> SortedNodes;
        CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoot(Nodes, SortedNodes, [](CEdGraphNode* Node)
        {
            return Cast<CParticleOutputNode>(Node) != nullptr;
        });

        if (CyclicNode != nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name          = "Cyclic";
            Error.Description   = "Cycle detected in particle node graph!";
            Error.Node          = CyclicNode;
            Compiler.AddError(Error);
            return;
        }

        for (size_t i = 0; i < SortedNodes.size(); ++i)
        {
            SortedNodes[i]->SetDebugExecutionOrder((uint32)i);
        }

        CParticleOutputNode* OutputNode = nullptr;
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (CParticleOutputNode* Out = Cast<CParticleOutputNode>(Node.Get()))
            {
                OutputNode = Out;
                break;
            }
        }

        if (OutputNode == nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name          = "NoOutput";
            Error.Description   = "Particle graph is missing an Output node.";
            Compiler.AddError(Error);
            return;
        }


        Compiler.SetContext(EParticleContext::Spawn);
        OutputNode->GenerateDefinition(Compiler);

        Compiler.SetContext(EParticleContext::Update);
        OutputNode->GenerateDefinition(Compiler);
    }

    void CParticleNodeGraph::ValidateGraph()
    {
        Connections.clear();
        Connections.reserve(16);

        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }
}
