#include "ParticleExpressionNodes.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    static EParticlePinType PromoteType(EParticlePinType A, EParticlePinType B)
    {
        if (A == B)                                 return A;
        if (A == EParticlePinType::Float4 || B == EParticlePinType::Float4) return EParticlePinType::Float4;
        if (A == EParticlePinType::Float3 || B == EParticlePinType::Float3) return EParticlePinType::Float3;
        return EParticlePinType::Float;
    }

    void CParticleExpression_ConstantFloat::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Value", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_ConstantFloat::GenerateDefinition(FParticleCompiler& Compiler)
    {
        Compiler.EmitCurrent("float " + GetNodeFullName() + " = " + FString(Format("{}", Value)) + ";");
    }

    void CParticleExpression_ConstantFloat3::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Value", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float3);
    }

    void CParticleExpression_ConstantFloat3::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FString Val = "float3(" + FString(Format("{}", Value.x)) + ", " + FString(Format("{}", Value.y)) + ", " + FString(Format("{}", Value.z)) + ")";
        Compiler.EmitCurrent("float3 " + GetNodeFullName() + " = " + Val + ";");
    }

    void CParticleExpression_ConstantFloat4::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Value", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float4);
    }

    void CParticleExpression_ConstantFloat4::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FString Val = "float4(" + FString(Format("{}", Value.x)) + ", " + FString(Format("{}", Value.y)) + ", " + FString(Format("{}", Value.z)) + ", " + FString(Format("{}", Value.w)) + ")";
        Compiler.EmitCurrent("float4 " + GetNodeFullName() + " = " + Val + ";");
    }

    void CParticleExpression_Time::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Time", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_Time::GenerateDefinition(FParticleCompiler& Compiler)
    {
        Compiler.EmitCurrent("float " + GetNodeFullName() + " = TotalTime;");
    }

    void CParticleExpression_ParticleAge::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Age", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_ParticleAge::GenerateDefinition(FParticleCompiler& Compiler)
    {
        // Spawn has no age yet; emit 0 so partial graphs still compile sensibly.
        if (Compiler.GetContext() == EParticleContext::Spawn)
        {
            Compiler.EmitSpawn("float " + GetNodeFullName() + " = 0.0;");
        }
        else
        {
            Compiler.EmitUpdate("float " + GetNodeFullName() + " = P.Age;");
        }
    }

    void CParticleExpression_LifeRatio::BuildNode()
    {
        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "LifeRatio", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_LifeRatio::GenerateDefinition(FParticleCompiler& Compiler)
    {
        if (Compiler.GetContext() == EParticleContext::Spawn)
        {
            Compiler.EmitSpawn("float " + GetNodeFullName() + " = 0.0;");
        }
        else
        {
            Compiler.EmitUpdate("float " + GetNodeFullName() + " = saturate(P.Age / max(P.Lifetime, 0.0001));");
        }
    }

    void CParticleExpression_BinaryOp::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float);

        B = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "B", ENodePinDirection::Input));
        B->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_BinaryOp::EmitBinary(FParticleCompiler& Compiler, const char* Op)
    {
        FParticleInputValue ValA = Compiler.GetInputFloat(A, 0.0f);
        FParticleInputValue ValB = Compiler.GetInputFloat(B, 0.0f);

        const EParticlePinType Type = PromoteType(ValA.Type, ValB.Type);
        const FString Left  = FParticleCompiler::Coerce(ValA, Type);
        const FString Right = FParticleCompiler::Coerce(ValB, Type);

        Compiler.EmitCurrent(FParticleCompiler::TypeName(Type) + " " + GetNodeFullName() + " = (" + Left + " " + Op + " " + Right + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(Type);
        }
    }

    void CParticleExpression_Lerp::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float);

        B = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "B", ENodePinDirection::Input));
        B->SetPinType(EParticlePinType::Float);

        Alpha = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "Alpha", ENodePinDirection::Input));
        Alpha->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_Lerp::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue ValA = Compiler.GetInputFloat(A, 0.0f);
        FParticleInputValue ValB = Compiler.GetInputFloat(B, 1.0f);
        FParticleInputValue ValT = Compiler.GetInputFloat(Alpha, 0.0f);

        const EParticlePinType Type = PromoteType(ValA.Type, ValB.Type);
        const FString Left  = FParticleCompiler::Coerce(ValA, Type);
        const FString Right = FParticleCompiler::Coerce(ValB, Type);
        const FString T     = FParticleCompiler::Coerce(ValT, EParticlePinType::Float);

        Compiler.EmitCurrent(FParticleCompiler::TypeName(Type) + " " + GetNodeFullName() + " = lerp(" + Left + ", " + Right + ", " + T + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(Type);
        }
    }

    void CParticleExpression_Saturate::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_Saturate::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue Val = Compiler.GetInputFloat(A, 0.0f);
        Compiler.EmitCurrent(FParticleCompiler::TypeName(Val.Type) + " " + GetNodeFullName() + " = saturate(" + Val.Value + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(Val.Type);
        }
    }

    void CParticleExpression_Sin::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_Sin::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue Val = Compiler.GetInputFloat(A, 0.0f);
        Compiler.EmitCurrent(FParticleCompiler::TypeName(Val.Type) + " " + GetNodeFullName() + " = sin(" + Val.Value + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(Val.Type);
        }
    }

    void CParticleExpression_Cos::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float);
    }

    void CParticleExpression_Cos::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue Val = Compiler.GetInputFloat(A, 0.0f);
        Compiler.EmitCurrent(FParticleCompiler::TypeName(Val.Type) + " " + GetNodeFullName() + " = cos(" + Val.Value + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(Val.Type);
        }
    }

    void CParticleExpression_Normalize::BuildNode()
    {
        A = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetPinType(EParticlePinType::Float3);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float3);
    }

    void CParticleExpression_Normalize::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue Val = Compiler.GetInputFloat3(A, FVector3(0.0f, 1.0f, 0.0f));
        const FString Expr = FParticleCompiler::Coerce(Val, EParticlePinType::Float3);
        Compiler.EmitCurrent("float3 " + GetNodeFullName() + " = normalize(" + Expr + ");");

        if (Output != nullptr)
        {
            Output->SetPinType(EParticlePinType::Float3);
        }
    }

    void CParticleExpression_MakeFloat3::BuildNode()
    {
        X = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "X", ENodePinDirection::Input));
        X->SetPinType(EParticlePinType::Float);

        Y = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "Y", ENodePinDirection::Input));
        Y->SetPinType(EParticlePinType::Float);

        Z = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "Z", ENodePinDirection::Input));
        Z->SetPinType(EParticlePinType::Float);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float3);
    }

    void CParticleExpression_MakeFloat3::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue ValX = Compiler.GetInputFloat(X, 0.0f);
        FParticleInputValue ValY = Compiler.GetInputFloat(Y, 0.0f);
        FParticleInputValue ValZ = Compiler.GetInputFloat(Z, 0.0f);

        const FString Sx = FParticleCompiler::Coerce(ValX, EParticlePinType::Float);
        const FString Sy = FParticleCompiler::Coerce(ValY, EParticlePinType::Float);
        const FString Sz = FParticleCompiler::Coerce(ValZ, EParticlePinType::Float);

        Compiler.EmitCurrent("float3 " + GetNodeFullName() + " = float3(" + Sx + ", " + Sy + ", " + Sz + ");");
    }

    void CParticleExpression_MakeFloat4::BuildNode()
    {
        X = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "X", ENodePinDirection::Input));
        X->SetPinType(EParticlePinType::Float);

        Y = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "Y", ENodePinDirection::Input));
        Y->SetPinType(EParticlePinType::Float);

        Z = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "Z", ENodePinDirection::Input));
        Z->SetPinType(EParticlePinType::Float);

        W = Cast<CParticleInput>(CreatePin(CParticleInput::StaticClass(), "W", ENodePinDirection::Input));
        W->SetPinType(EParticlePinType::Float);
        W->SetDefaultFloat(1.0f);

        Output = Cast<CParticleOutput>(CreatePin(CParticleOutput::StaticClass(), "Result", ENodePinDirection::Output));
        Output->SetPinType(EParticlePinType::Float4);
    }

    void CParticleExpression_MakeFloat4::GenerateDefinition(FParticleCompiler& Compiler)
    {
        FParticleInputValue ValX = Compiler.GetInputFloat(X, 0.0f);
        FParticleInputValue ValY = Compiler.GetInputFloat(Y, 0.0f);
        FParticleInputValue ValZ = Compiler.GetInputFloat(Z, 0.0f);
        FParticleInputValue ValW = Compiler.GetInputFloat(W, 1.0f);

        const FString Sx = FParticleCompiler::Coerce(ValX, EParticlePinType::Float);
        const FString Sy = FParticleCompiler::Coerce(ValY, EParticlePinType::Float);
        const FString Sz = FParticleCompiler::Coerce(ValZ, EParticlePinType::Float);
        const FString Sw = FParticleCompiler::Coerce(ValW, EParticlePinType::Float);

        Compiler.EmitCurrent("float4 " + GetNodeFullName() + " = float4(" + Sx + ", " + Sy + ", " + Sz + ", " + Sw + ");");
    }
}
