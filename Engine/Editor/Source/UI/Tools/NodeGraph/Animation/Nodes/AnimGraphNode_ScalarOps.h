#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "AnimGraphNode_ScalarOps.generated.h"

namespace Lumina
{
    // Shared base for binary scalar nodes (A op B). Subclasses only declare the
    // EAnimScalarOp they emit; pin building and bytecode emission live here.
    REFLECT()
    class CAnimGraphNode_ScalarBinaryOp : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FFixedString GetNodeCategory() const override { return "Math"; }

        virtual EAnimScalarOp GetScalarOp() const { return EAnimScalarOp::Add; }

        // Defaults fed into unconnected input pins; multiply/divide override B to
        // 1 so a half-wired node behaves sensibly.
        virtual float GetDefaultA() const { return 0.0f; }
        virtual float GetDefaultB() const { return 0.0f; }

        CAnimGraphPin* APin = nullptr;
        CAnimGraphPin* BPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };

    // Shared base for unary scalar nodes (op A).
    REFLECT()
    class CAnimGraphNode_ScalarUnaryOp : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FFixedString GetNodeCategory() const override { return "Math"; }

        virtual EAnimScalarOp GetScalarOp() const { return EAnimScalarOp::Clamp01; }
        virtual float GetDefaultA() const { return 0.0f; }

        CAnimGraphPin* APin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };

    REFLECT()
    class CAnimGraphNode_Add : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Add"; }
        FStringView GetNodeTooltip() const override { return "Returns A + B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Add; }
    };

    REFLECT()
    class CAnimGraphNode_Subtract : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Subtract"; }
        FStringView GetNodeTooltip() const override { return "Returns A - B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Sub; }
    };

    REFLECT()
    class CAnimGraphNode_Multiply : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Multiply"; }
        FStringView GetNodeTooltip() const override { return "Returns A * B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Mul; }
        float GetDefaultA() const override { return 1.0f; }
        float GetDefaultB() const override { return 1.0f; }
    };

    REFLECT()
    class CAnimGraphNode_Divide : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Divide"; }
        FStringView GetNodeTooltip() const override { return "Returns A / B. Division by zero yields 0."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Div; }
        float GetDefaultB() const override { return 1.0f; }
    };

    REFLECT()
    class CAnimGraphNode_Min : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Min"; }
        FStringView GetNodeTooltip() const override { return "Returns the lesser of A and B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Min; }
    };

    REFLECT()
    class CAnimGraphNode_Max : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Max"; }
        FStringView GetNodeTooltip() const override { return "Returns the greater of A and B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Max; }
    };

    REFLECT()
    class CAnimGraphNode_Clamp01 : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Clamp 01"; }
        FStringView GetNodeTooltip() const override { return "Clamps A into the [0, 1] range."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Clamp01; }
    };

    REFLECT()
    class CAnimGraphNode_OneMinus : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "One Minus"; }
        FStringView GetNodeTooltip() const override { return "Returns 1 - A. Handy for inverting a blend alpha."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::OneMinus; }
    };

    REFLECT()
    class CAnimGraphNode_AbsoluteValue : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Absolute Value"; }
        FStringView GetNodeTooltip() const override { return "Returns the absolute value of A."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Abs; }
    };

    REFLECT()
    class CAnimGraphNode_Sine : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Sine"; }
        FStringView GetNodeTooltip() const override { return "Returns sin(A), with A in radians."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Sin; }
    };

    REFLECT()
    class CAnimGraphNode_Cosine : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Cosine"; }
        FStringView GetNodeTooltip() const override { return "Returns cos(A), with A in radians."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Cos; }
    };

    REFLECT()
    class CAnimGraphNode_Modulo : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Modulo"; }
        FStringView GetNodeTooltip() const override { return "Returns A mod B (fmod). B of zero yields 0."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Mod; }
        float GetDefaultB() const override { return 1.0f; }
    };

    REFLECT()
    class CAnimGraphNode_Power : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Power"; }
        FStringView GetNodeTooltip() const override { return "Returns A raised to the power B."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Pow; }
        float GetDefaultA() const override { return 1.0f; }
        float GetDefaultB() const override { return 1.0f; }
    };

    REFLECT()
    class CAnimGraphNode_Atan2 : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Atan2"; }
        FStringView GetNodeTooltip() const override { return "Returns atan2(A, B) in radians. Useful for aim angles."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Atan2; }
    };

    REFLECT()
    class CAnimGraphNode_Less : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Less"; }
        FStringView GetNodeTooltip() const override { return "Returns 1 when A < B, else 0. Feeds blend alphas and transition parameters."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Less; }
    };

    REFLECT()
    class CAnimGraphNode_Greater : public CAnimGraphNode_ScalarBinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Greater"; }
        FStringView GetNodeTooltip() const override { return "Returns 1 when A > B, else 0. Feeds blend alphas and transition parameters."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Greater; }
    };

    REFLECT()
    class CAnimGraphNode_Floor : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Floor"; }
        FStringView GetNodeTooltip() const override { return "Rounds A down to the nearest integer."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Floor; }
    };

    REFLECT()
    class CAnimGraphNode_Ceil : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Ceil"; }
        FStringView GetNodeTooltip() const override { return "Rounds A up to the nearest integer."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Ceil; }
    };

    REFLECT()
    class CAnimGraphNode_Frac : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Frac"; }
        FStringView GetNodeTooltip() const override { return "Returns the fractional part of A (A - floor(A))."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Frac; }
    };

    REFLECT()
    class CAnimGraphNode_SquareRoot : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Square Root"; }
        FStringView GetNodeTooltip() const override { return "Returns sqrt(A). Negative A yields 0."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Sqrt; }
    };

    REFLECT()
    class CAnimGraphNode_Negate : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Negate"; }
        FStringView GetNodeTooltip() const override { return "Returns -A."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Negate; }
    };

    REFLECT()
    class CAnimGraphNode_Sign : public CAnimGraphNode_ScalarUnaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Sign"; }
        FStringView GetNodeTooltip() const override { return "Returns -1, 0, or 1 depending on the sign of A."; }
        EAnimScalarOp GetScalarOp() const override { return EAnimScalarOp::Sign; }
    };
}
