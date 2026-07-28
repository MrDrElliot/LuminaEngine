#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Conditional.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_If : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Conditional"; }
        FStringView GetNodeDisplayName() const override { return "If"; }
        FStringView GetNodeTooltip() const override { return "Selects between three values based on the relation of X and Y. AGreater used when X > Y, AEqual when |X-Y|<Threshold, ALess when X < Y."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* X = nullptr;
        CMaterialInput* Y = nullptr;
        CMaterialInput* AGreater = nullptr;
        CMaterialInput* AEqual = nullptr;
        CMaterialInput* ALess = nullptr;

        /** Equality tolerance, values within +/- Threshold of each other are considered equal. */
        PROPERTY(Editable) float Threshold = 0.00001f;
    };

    REFLECT()
    enum class ECompareOp : uint8
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
    };

    REFLECT()
    class CMaterialExpression_Compare : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Conditional"; }
        FStringView GetNodeDisplayName() const override { return "Compare"; }
        FStringView GetNodeTooltip() const override { return "Returns 1.0 if the comparison succeeds, 0.0 otherwise."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* A = nullptr;
        CMaterialInput* B = nullptr;

        PROPERTY(Editable) ECompareOp Op = ECompareOp::Greater;
    };
}
