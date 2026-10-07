#include "EditorPCH.h"
#include "Material/MaterialOps.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "Core/Object/ObjectIterator.h"
#include "UI/Tools/NodeGraph/Material/MaterialFunctionGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialOutputNode.h"

namespace Lumina::MaterialOps
{
    namespace
    {
        // A graph created since the last save is not in the export table LoadObjectByName reads.
        template<typename GraphType>
        GraphType* FindLiveGraph(CPackage* Package, FName GraphName)
        {
            for (TObjectIterator<GraphType> It; It; ++It)
            {
                if ((*It)->GetPackage() == Package && (*It)->GetName() == GraphName && !(*It)->HasAnyFlag(OF_MarkedDestroy))
                {
                    return *It;
                }
            }

            return Cast<GraphType>(Package->LoadObjectByName(GraphName));
        }
    }

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

        for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
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

        if (CMaterialNodeGraph* Existing = FindLiveGraph<CMaterialNodeGraph>(Package, FName(GraphName)))
        {
            return Existing;
        }

        CMaterialNodeGraph* Graph = NewObject<CMaterialNodeGraph>(Package, GraphName);
        Graph->SetMaterial(Material);
        Graph->CreateNode(CMaterialOutputNode::StaticClass());

        return Graph;
    }

    CMaterialNodeGraph* FindOrCreateFunctionGraph(CMaterialFunction* Function)
    {
        CPackage* Package = Function != nullptr ? Function->GetPackage() : nullptr;
        if (Package == nullptr)
        {
            return nullptr;
        }

        const FName GraphName(GMaterialFunctionGraphObjectName);
        if (CMaterialFunctionGraph* Existing = FindLiveGraph<CMaterialFunctionGraph>(Package, GraphName))
        {
            return Existing;
        }

        return NewObject<CMaterialFunctionGraph>(Package, GraphName);
    }
}
