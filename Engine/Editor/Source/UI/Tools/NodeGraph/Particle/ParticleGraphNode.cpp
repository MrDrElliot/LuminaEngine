#include "ParticleGraphNode.h"

#include "ParticleNodeGraph.h"

namespace Lumina
{
    CClass* CParticleGraphNode::GetSupportedGraphClass() const
    {
        return CParticleNodeGraph::StaticClass();
    }
}
