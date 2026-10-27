#include "ParticleNodeGraph.h"
#include "ParticleCompiler.h"
#include "ParticleGraphNode.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"

namespace Lumina
{
    void CParticleNodeGraph::CompileModule(FParticleCompiler& Compiler, EParticleContext Context)
    {
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            Node->ClearError();
        }

        auto IsSink = [](CEdGraphNode* Node)
        {
            const CParticleGraphNode* ParticleNode = Cast<CParticleGraphNode>(Node);
            return ParticleNode != nullptr && ParticleNode->IsSink();
        };

        TVector<CEdGraphNode*> SortedNodes;
        if (CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoots(Nodes, SortedNodes, IsSink))
        {
            Compiler.AddError({ "Cyclic", "The module graph loops back on itself.", CyclicNode });
            return;
        }

        TVector<CParticleGraphNode*> Sinks;
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (IsSink(Node.Get()))
            {
                Sinks.push_back(Cast<CParticleGraphNode>(Node.Get()));
            }
        }
        Algo::Sort(Sinks, [](const CParticleGraphNode* A, const CParticleGraphNode* B)
        {
            return A->GetNodeY() != B->GetNodeY() ? A->GetNodeY() < B->GetNodeY() : A->GetNodeX() < B->GetNodeX();
        });

        // Scoped, so two graph modules in one stage can reuse node names.
        const EParticleContext Saved = Compiler.GetContext();
        Compiler.SetContext(Context);
        Compiler.Emit(Context, "{");
        for (CParticleGraphNode* Sink : Sinks)
        {
            Compiler.EnsureEmitted(Sink, Context);
        }
        for (CParticleGraphNode* Sink : Sinks)
        {
            Sink->EmitWrites(Compiler);
        }
        Compiler.Emit(Context, "}");
        Compiler.SetContext(Saved);
    }

    void CParticleNodeGraph::ValidateGraph()
    {
        Connections.clear();
        Connections.reserve(16);

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }
}
