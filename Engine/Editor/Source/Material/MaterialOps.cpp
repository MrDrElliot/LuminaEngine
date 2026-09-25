#include "EditorPCH.h"
#include "Material/MaterialOps.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialOutputNode.h"

namespace Lumina::MaterialOps
{
    TVector<CClass*> GetPlaceableNodeTypes()
    {
        return NodeGraphOps::GetPlaceableNodeTypes(CMaterialNodeGraph::StaticClass());
    }

    CClass* ResolveNodeType(FStringView TypeName)
    {
        return NodeGraphOps::ResolveNodeType(CMaterialNodeGraph::StaticClass(), TypeName);
    }

    CEdGraphNode* FindOutputNode(CMaterialNodeGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }

        for (const TObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
        {
            if (Node.IsValid() && Node->IsA<CMaterialOutputNode>())
            {
                return Node.Get();
            }
        }

        return nullptr;
    }

    CMaterialNodeGraph* FindOrCreateGraph(CMaterial* Material)
    {
        if (Material == nullptr)
        {
            return nullptr;
        }

        CPackage* Package = Material->GetPackage();
        if (Package == nullptr)
        {
            return nullptr;
        }

        // The name the material editor looks for, so an agent-built graph opens as the same graph.
        const FString GraphName = "AssetMaterialGraph";

        if (CMaterialNodeGraph* Existing = Cast<CMaterialNodeGraph>(Package->LoadObjectByName(GraphName)))
        {
            return Existing;
        }

        CMaterialNodeGraph* Graph = NewObject<CMaterialNodeGraph>(Package, GraphName);
        Graph->SetMaterial(Material);
        Graph->CreateNode(CMaterialOutputNode::StaticClass());

        return Graph;
    }
}
