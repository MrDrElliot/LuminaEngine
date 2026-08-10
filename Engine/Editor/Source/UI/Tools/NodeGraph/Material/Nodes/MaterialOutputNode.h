#pragma once

#include "MaterialGraphNode.h"
#include "MaterialOutputNode.generated.h"

namespace Lumina
{
    // The graph creates exactly one of these itself (EnsureRootNodes), so it must never be offered
    // in the palette -- a second one would give the compiler two roots.
    REFLECT(NotPlaceable)
    class CMaterialOutputNode : public CMaterialGraphNode
    {
        GENERATED_BODY()
    public:
        
        CMaterialOutputNode() = default;

        FStringView GetNodeDisplayName() const override;
        FStringView GetNodeTooltip() const override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(35, 35, 200, 255); }

        void BuildNode() override;

        // Refreshes pin enable/disable from the current material domain before pins draw, so the right
        // pins light up immediately after flipping MaterialType in the inspector.
        void DrawNodeTitleBar() override;

        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /**
         * Every pin GenerateDefinition assigns from in the PIXEL stage, in declaration order.
         *
         * Single source of truth on purpose. The graph compiler consumes this twice -- once to collect the
         * node closure that gets emitted, once as the roots of the emit ordering -- and GenerateDefinition
         * writes an assignment for each of them unconditionally. A pin present in the assignments but
         * missing here does not degrade to "feature ignored": the assignment still references the upstream
         * node's variable, which was never declared, and the whole material fails to compile.
         *
         * WorldPositionOffsetPin is deliberately absent; it is the vertex stage's only root.
         */
        void GetPixelStagePins(TVector<CEdNodeGraphPin*>& OutPins) const;

        bool IsDeletable() const override { return false; }

        CEdNodeGraphPin* BaseColorPin = nullptr;
        CEdNodeGraphPin* MetallicPin = nullptr;
        CEdNodeGraphPin* RoughnessPin = nullptr;
        CEdNodeGraphPin* SpecularPin = nullptr;
        CEdNodeGraphPin* EmissivePin = nullptr;
        CEdNodeGraphPin* AOPin = nullptr;
        CEdNodeGraphPin* NormalPin = nullptr;
        CEdNodeGraphPin* OpacityPin = nullptr;
        CEdNodeGraphPin* SelfShadowPin = nullptr;
        CEdNodeGraphPin* ClearcoatPin = nullptr;
        CEdNodeGraphPin* ClearcoatRoughnessPin = nullptr;
        CEdNodeGraphPin* WorldPositionOffsetPin = nullptr;

    };
    
}
