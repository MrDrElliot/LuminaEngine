#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "ParticleNodeGraph.generated.h"

namespace Lumina
{
    class FParticleCompiler;
    enum class EParticleContext : uint8;
}

namespace Lumina
{
    // The graph a Graph Module runs, whose sink nodes write particle, grid or render target values.
    REFLECT()
    class EDITOR_API CParticleNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()

    public:

        // Reads every sink's inputs before any sink writes, so each one sees the values from before the module, top to bottom on the canvas.
        void CompileModule(FParticleCompiler& Compiler, EParticleContext Context);

        void ValidateGraph() override;
        bool ShouldDrawInlinePinEditors() const override { return true; }
    };
}
