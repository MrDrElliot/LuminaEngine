#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "ParticlePin.h"
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
    class EDITOR_API CParticleGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Every particle node belongs to the particle graph. Declared once here so a node in a game
        // or plugin module needs nothing but this base to appear in the palette.
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateDefinition(FParticleCompiler& Compiler) { }

        // A sink writes particle, grid or target values, after every sink's inputs have been read.
        virtual bool IsSink() const { return false; }
        virtual void EmitWrites(FParticleCompiler& Compiler) { }

        // Unwired input values by pin index, saved here because pins are rebuilt on load.
        PROPERTY(Hidden)
        TVector<FVector4> PinDefaults;

    protected:

        CParticleInput*  AddInput(const char* Name, EParticlePinType Type, const FVector4& Default = FVector4(0.0f));
        CParticleOutput* AddOutput(const char* Name, EParticlePinType Type);
    };
}
