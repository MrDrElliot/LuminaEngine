#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Constants.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_Constant : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        FFixedString GetNodeCategory() const override { return "Constants"; }
        void DrawContextMenu() override;
        void BuildNode() override;

        void* GetNodeDefaultValue() override { return &Value.r; }
        FName* GetParameterName() override { return &ParameterName; }

        /** Name used to expose this constant as a material parameter for instancing. */
        PROPERTY(Editable, Category = "Parameter")
        FName               ParameterName;

        /** Default value of the constant, also used as the parameter default. */
        PROPERTY(Editable, Color, Category = "Value")
        FVector4           Value = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

        EMaterialInputType  ValueType = EMaterialInputType::Float;
    };

    REFLECT()
    class CMaterialExpression_ConstantFloat : public CMaterialExpression_Constant
    {
        GENERATED_BODY()
    public:

        CMaterialExpression_ConstantFloat() { ValueType = EMaterialInputType::Float; }

        FStringView GetNodeDisplayName() const override { return "Float"; }
        FStringView GetNodeTooltip() const override { return "Scalar constant. Right-click to expose as a runtime parameter."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void DrawNodeBody() override;
    };

    REFLECT()
    class CMaterialExpression_ConstantFloat2 : public CMaterialExpression_Constant
    {
        GENERATED_BODY()
    public:

        CMaterialExpression_ConstantFloat2() { ValueType = EMaterialInputType::Float2; }

        FStringView GetNodeDisplayName() const override { return "Float2"; }
        FStringView GetNodeTooltip() const override { return "Two-component constant (X, Y). Right-click to expose as a runtime parameter."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void DrawNodeBody() override;
    };

    REFLECT()
    class CMaterialExpression_ConstantFloat3 : public CMaterialExpression_Constant
    {
        GENERATED_BODY()
    public:

        CMaterialExpression_ConstantFloat3() { ValueType = EMaterialInputType::Float3; }

        FStringView GetNodeDisplayName() const override { return "Float3"; }
        FStringView GetNodeTooltip() const override { return "Three-component constant (R, G, B). Right-click to expose as a runtime parameter."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void DrawNodeBody() override;
    };

    REFLECT()
    class CMaterialExpression_ConstantFloat4 : public CMaterialExpression_Constant
    {
        GENERATED_BODY()
    public:

        CMaterialExpression_ConstantFloat4() { ValueType = EMaterialInputType::Float4; }

        FStringView GetNodeDisplayName() const override { return "Float4"; }
        FStringView GetNodeTooltip() const override { return "Four-component constant (R, G, B, A). Right-click to expose as a runtime parameter."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void DrawNodeBody() override;
    };

    REFLECT()
    enum class EBuiltinConstant : uint8
    {
        Pi,
        TwoPi,
        HalfPi,
        E,
        Sqrt2,
        GoldenRatio,
        DegToRad,
        RadToDeg,
        Zero,
        One,
    };

    REFLECT()
    class CMaterialExpression_NumericConstant : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        FFixedString GetNodeCategory() const override { return "Constants"; }
        FStringView GetNodeDisplayName() const override;
        FStringView GetNodeTooltip() const override { return "Built-in mathematical constant (Pi, Tau, Half-Pi, e, etc)."; }
        void BuildNode() override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Which built-in constant this node emits. */
        PROPERTY(Editable)
        EBuiltinConstant Constant = EBuiltinConstant::Pi;
    };
}
