#include "MaterialNode_PrimitiveData.h"


namespace Lumina
{
    FStringView CMaterialExpression_CustomPrimitiveData::GetNodeTooltip() const
    {
        return "Retrieves custom primitive data and interprets it as the selected type";
    }

    void CMaterialExpression_CustomPrimitiveData::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (!Compiler.RequirePixelStage(this, "CustomPrimitiveData")) return;
        Compiler.CustomPrimitiveData(this, Type);
    }
    
}
