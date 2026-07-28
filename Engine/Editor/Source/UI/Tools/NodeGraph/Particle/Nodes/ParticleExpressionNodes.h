#pragma once

#include "UI/Tools/NodeGraph/Particle/ParticleGraphNode.h"
#include "UI/Tools/NodeGraph/Particle/ParticlePin.h"
#include "ParticleExpressionNodes.generated.h"

namespace Lumina
{
    REFLECT()
    class CParticleExpression_ConstantFloat : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "ConstantFloat"; }
        FStringView GetNodeTooltip() const override { return "Outputs a constant float value."; }
        FFixedString GetNodeCategory() const override { return "Constants"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Value")
        float Value = 0.0f;

        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_ConstantFloat3 : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "ConstantFloat3"; }
        FStringView GetNodeTooltip() const override { return "Outputs a constant float3 value."; }
        FFixedString GetNodeCategory() const override { return "Constants"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Value")
        FVector3 Value = FVector3(0.0f);

        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_ConstantFloat4 : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "ConstantFloat4"; }
        FStringView GetNodeTooltip() const override { return "Outputs a constant float4 value."; }
        FFixedString GetNodeCategory() const override { return "Constants"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Value")
        FVector4 Value = FVector4(1.0f);

        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_Time : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Time"; }
        FStringView GetNodeTooltip() const override { return "Outputs the total simulation time in seconds."; }
        FFixedString GetNodeCategory() const override { return "Inputs"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_ParticleAge : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "ParticleAge"; }
        FStringView GetNodeTooltip() const override { return "Current age of the particle in seconds."; }
        FFixedString GetNodeCategory() const override { return "Inputs"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_LifeRatio : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "LifeRatio"; }
        FStringView GetNodeTooltip() const override { return "Normalized particle age from 0 (born) to 1 (about to die)."; }
        FFixedString GetNodeCategory() const override { return "Inputs"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleOutput* Output = nullptr;
    };

    /** Shared base for binary math operators. Both inputs get promoted to a common type. */
    REFLECT()
    class CParticleExpression_BinaryOp : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;

        CParticleInput*  A      = nullptr;
        CParticleInput*  B      = nullptr;
        CParticleOutput* Output = nullptr;

    protected:

        /** Emits "<type> <NodeFullName> = (<a> <op> <b>);" in the current context. */
        void EmitBinary(FParticleCompiler& Compiler, const char* Op);
    };

    REFLECT()
    class CParticleExpression_Add : public CParticleExpression_BinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Add"; }
        FStringView GetNodeTooltip() const override { return "Component-wise addition."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
        void GenerateDefinition(FParticleCompiler& Compiler) override { EmitBinary(Compiler, "+"); }
    };

    REFLECT()
    class CParticleExpression_Subtract : public CParticleExpression_BinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Subtract"; }
        FStringView GetNodeTooltip() const override { return "Component-wise subtraction."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
        void GenerateDefinition(FParticleCompiler& Compiler) override { EmitBinary(Compiler, "-"); }
    };

    REFLECT()
    class CParticleExpression_Multiply : public CParticleExpression_BinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Multiply"; }
        FStringView GetNodeTooltip() const override { return "Component-wise multiplication."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
        void GenerateDefinition(FParticleCompiler& Compiler) override { EmitBinary(Compiler, "*"); }
    };

    REFLECT()
    class CParticleExpression_Divide : public CParticleExpression_BinaryOp
    {
        GENERATED_BODY()
    public:
        FStringView GetNodeDisplayName() const override { return "Divide"; }
        FStringView GetNodeTooltip() const override { return "Component-wise division."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
        void GenerateDefinition(FParticleCompiler& Compiler) override { EmitBinary(Compiler, "/"); }
    };

    REFLECT()
    class CParticleExpression_Lerp : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Lerp"; }
        FStringView GetNodeTooltip() const override { return "Linear interpolation: A + (B - A) * Alpha."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  A      = nullptr;
        CParticleInput*  B      = nullptr;
        CParticleInput*  Alpha  = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_Saturate : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Saturate"; }
        FStringView GetNodeTooltip() const override { return "Clamps each component to [0, 1]."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  A      = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_Sin : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Sin"; }
        FStringView GetNodeTooltip() const override { return "Sine of the input (radians)."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  A      = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_Cos : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Cos"; }
        FStringView GetNodeTooltip() const override { return "Cosine of the input (radians)."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  A      = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_Normalize : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Normalize"; }
        FStringView GetNodeTooltip() const override { return "Normalizes a vector to unit length."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  A      = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_MakeFloat3 : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "MakeFloat3"; }
        FStringView GetNodeTooltip() const override { return "Packs three scalars into a float3."; }
        FFixedString GetNodeCategory() const override { return "Vector"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  X      = nullptr;
        CParticleInput*  Y      = nullptr;
        CParticleInput*  Z      = nullptr;
        CParticleOutput* Output = nullptr;
    };

    REFLECT()
    class CParticleExpression_MakeFloat4 : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "MakeFloat4"; }
        FStringView GetNodeTooltip() const override { return "Packs four scalars into a float4."; }
        FFixedString GetNodeCategory() const override { return "Vector"; }

        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        CParticleInput*  X      = nullptr;
        CParticleInput*  Y      = nullptr;
        CParticleInput*  Z      = nullptr;
        CParticleInput*  W      = nullptr;
        CParticleOutput* Output = nullptr;
    };
}
