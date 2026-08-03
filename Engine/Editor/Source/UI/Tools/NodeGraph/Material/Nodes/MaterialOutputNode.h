#pragma once

#include "MaterialGraphNode.h"
#include "MaterialOutputNode.generated.h"

namespace Lumina
{
    REFLECT()
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
        CEdNodeGraphPin* WorldPositionOffsetPin = nullptr;

    };
    
}
