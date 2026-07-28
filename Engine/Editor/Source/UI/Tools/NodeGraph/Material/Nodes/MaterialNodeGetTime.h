#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectMacros.h"
#include "MaterialNodeGetTime.generated.h"



namespace Lumina
{
    REFLECT()
    class CMaterialNodeGetTime : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Utility"; }
        FStringView GetNodeDisplayName() const override { return "Time"; }
        void* GetNodeDefaultValue() override { return nullptr; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    
    };
}
