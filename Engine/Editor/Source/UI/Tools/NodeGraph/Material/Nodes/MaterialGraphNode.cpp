#include "MaterialGraphNode.h"

#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"

namespace Lumina
{
    CClass* CMaterialGraphNode::GetSupportedGraphClass() const
    {
        return CMaterialNodeGraph::StaticClass();
    }

    void CMaterialGraphNode::PostPropertyChange(FProperty* ChangedProperty)
    {
        
    }
}
