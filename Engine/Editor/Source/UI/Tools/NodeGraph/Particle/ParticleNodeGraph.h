#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "ParticleNodeGraph.generated.h"

namespace Lumina
{
    class CParticleSystem;
    class FParticleCompiler;
}

namespace Lumina
{
    REFLECT()
    class CParticleNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()

    public:

        void Initialize() override;
        void Shutdown() override;

        void CompileGraph(FParticleCompiler& Compiler);

        void ValidateGraph() override;

        void SetParticleSystem(CParticleSystem* InSystem) { ParticleSystem = InSystem; }
        CParticleSystem* GetParticleSystem() const { return ParticleSystem.Get(); }

    private:

        TObjectPtr<CParticleSystem> ParticleSystem;
    };
}
