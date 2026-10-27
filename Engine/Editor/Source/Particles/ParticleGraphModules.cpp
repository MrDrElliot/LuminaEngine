#include "ParticleGraphModules.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "UI/Tools/NodeGraph/Particle/Nodes/ParticleGraphNodes.h"

namespace Lumina
{
    // Places a node of class T at a canvas position.
    template<typename T>
    static T* PlaceNode(CParticleNodeGraph* Graph, float X, float Y)
    {
        T* Node = Cast<T>(Graph->CreateNode(T::StaticClass()));
        Node->SetGridPos(X, Y);
        return Node;
    }

    // Wires an output of one node into an input of another, by pin index.
    static void Wire(CParticleNodeGraph* Graph, CEdGraphNode* From, int32 Output, CEdGraphNode* To, int32 Input)
    {
        Graph->ConnectPins(From->GetOutputPins()[Output].Get(), To->GetInputPins()[Input].Get());
    }

    void CParticleModule_Graph::GetOwnedObjects(TVector<CObject*>& OutObjects) const
    {
        if (Graph.IsValid())
        {
            OutObjects.push_back(Graph.Get());
            for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
            {
                OutObjects.push_back(Node.Get());
            }
        }
    }

    CParticleNodeGraph* CParticleModule_Graph::GetGraph()
    {
        if (!Graph.IsValid())
        {
            Graph = NewObject<CParticleNodeGraph>(GetPackage());
            SeedGraph(Graph.Get());
        }
        return Graph.Get();
    }

    void CParticleModule_Graph::GenerateInto(FParticleCompiler& Compiler, EParticleContext Context)
    {
        GetGraph()->CompileModule(Compiler, Context);
    }

    void CParticleModule_GraphSpawn::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Spawn);
    }

    void CParticleModule_GraphSpawn::SeedGraph(CParticleNodeGraph* NewGraph)
    {
        PlaceNode<CParticleNode_SetParticle>(NewGraph, 300.0f, 0.0f);
    }

    void CParticleModule_GraphUpdate::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Update);
    }

    void CParticleModule_GraphUpdate::SeedGraph(CParticleNodeGraph* NewGraph)
    {
        PlaceNode<CParticleNode_Particle>(NewGraph, 0.0f, 0.0f);
        PlaceNode<CParticleNode_SetParticle>(NewGraph, 400.0f, 0.0f);
    }

    void CParticleModule_GraphGrid::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Grid);
    }

    void CParticleModule_GraphGrid::SeedGraph(CParticleNodeGraph* NewGraph)
    {
        CParticleNode_Cell* Cell = PlaceNode<CParticleNode_Cell>(NewGraph, 0.0f, 0.0f);
        Wire(NewGraph, Cell, 0, PlaceNode<CParticleNode_SetCell>(NewGraph, 400.0f, 0.0f), 0);
    }

    void CParticleModule_GraphTarget::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Target);
    }

    void CParticleModule_GraphTarget::SeedGraph(CParticleNodeGraph* NewGraph)
    {
        PlaceNode<CParticleNode_SetTargetColor>(NewGraph, 400.0f, 0.0f);
    }
}
