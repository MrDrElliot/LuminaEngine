#include "ParticleGraphNode.h"

#include "ParticleNodeGraph.h"
#include "Core/Object/Cast.h"

namespace Lumina
{
    CClass* CParticleGraphNode::GetSupportedGraphClass() const
    {
        return CParticleNodeGraph::StaticClass();
    }

    CParticleInput* CParticleGraphNode::AddInput(const char* Name, EParticlePinType Type, const FVector4& Default)
    {
        CParticleInput* Pin = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), Name, ENodePinDirection::Input));
        Pin->SetPinType(Type);
        Pin->SetDefaultFloat4(Default);
        return Pin;
    }

    CParticleOutput* CParticleGraphNode::AddOutput(const char* Name, EParticlePinType Type)
    {
        CParticleOutput* Pin = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), Name, ENodePinDirection::Output));
        Pin->SetPinType(Type);
        return Pin;
    }
}
