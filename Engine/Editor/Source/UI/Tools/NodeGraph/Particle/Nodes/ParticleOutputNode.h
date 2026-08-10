#pragma once

#include "UI/Tools/NodeGraph/Particle/ParticleGraphNode.h"
#include "UI/Tools/NodeGraph/Particle/ParticlePin.h"
#include "ParticleOutputNode.generated.h"

namespace Lumina
{
    class FParticleCompiler;
}

namespace Lumina
{
    // The graph creates its one output node itself; never palette-placeable.
    REFLECT(NotPlaceable)
    class CParticleOutputNode : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Particle Output"; }
        FStringView GetNodeTooltip() const override { return "Final output parameters for spawn and simulation."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(30, 130, 200, 255); }
        bool IsDeletable() const override { return false; }
        FFixedString GetNodeCategory() const override { return "Output"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

    public:

        CParticleInput* InitialVelocityPin  = nullptr;
        CParticleInput* LifetimePin         = nullptr;
        CParticleInput* GravityPin          = nullptr;
        CParticleInput* StartColorPin       = nullptr;
        CParticleInput* StartSizePin        = nullptr;
    };
}
