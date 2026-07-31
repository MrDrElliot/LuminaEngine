#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "MaterialNode_CurveSample.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_CurveSample : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Curves"; }
        FStringView GetNodeDisplayName() const override { return "Curve Sample"; }
        FStringView GetNodeTooltip() const override { return "Samples a curve asset by time. The curve is baked into shader constants when the material compiles."; }
        void* GetNodeDefaultValue() override { return &Curve; }
        void SetNodeValue(void* Value) override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void DrawNodeBody() override;

        /** The curve asset sampled by this node. */
        PROPERTY(Editable, Category = "Curve")
        TObjectPtr<CCurveAsset> Curve;

        CMaterialInput* Time = nullptr;
    };
}
