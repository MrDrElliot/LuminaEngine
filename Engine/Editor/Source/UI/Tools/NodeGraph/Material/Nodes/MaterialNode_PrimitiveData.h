#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectMacros.h"
#include "Renderer/CustomPrimitiveData.h"
#include "MaterialNode_PrimitiveData.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_CustomPrimitiveData : public CMaterialExpression
    {
        GENERATED_BODY()
        
    public:
        
        ImVec2 GetMinNodeTitleBarSize() const override { return ImVec2(60, 28); }
        FFixedString GetNodeCategory() const override { return "Utility"; }
        FStringView GetNodeDisplayName() const override { return "CustomPrimitiveData"; }
        FStringView GetNodeTooltip() const override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        
        /** Data type of the custom primitive data slot to read from the instance. */
        PROPERTY(Editable)
        ECustomPrimitiveDataType Type = ECustomPrimitiveDataType::Float;
        
    };
}
