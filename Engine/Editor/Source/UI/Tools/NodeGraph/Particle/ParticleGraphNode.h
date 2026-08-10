#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "ParticleGraphNode.generated.h"

namespace Lumina
{
    class FParticleCompiler;
}

namespace Lumina
{
    // NotPlaceable: family base, not a node. The specifier does not inherit, so concrete particle
    // nodes below stay discoverable.
    REFLECT(NotPlaceable)
    class CParticleGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Every particle node belongs to the particle graph. Declared once here so a node in a game
        // or plugin module needs nothing but this base to appear in the palette.
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateDefinition(FParticleCompiler& Compiler) { }
    };
}
