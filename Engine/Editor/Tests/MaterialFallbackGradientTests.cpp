#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Triplanar.h"

using namespace Lumina;

namespace
{
    // Any unconnected input will do, since only the fallback text decides the gradient.
    struct FFallbackFixture
    {
        FMaterialCompiler Compiler;
        CMaterialExpression_TriplanarSample* Node = nullptr;

        FFallbackFixture()
        {
            Node = NewObject<CMaterialExpression_TriplanarSample>();
            Node->BuildNode();
        }

        FMaterialCompiler::FInputValue Resolve(EMaterialCompileStage Stage, const char* Fallback)
        {
            Compiler.SetStage(Stage);
            return Compiler.GetTypedInputValue(Node->Position, FString(Fallback));
        }
    };
}

TEST(MaterialFallbackGradient, UVFallbackCarriesUV0Gradients)
{
    FFallbackFixture F;
    for (const char* Fallback : { "float2(UV0)", "UV0" })
    {
        const FMaterialCompiler::FInputValue Value = F.Resolve(EMaterialCompileStage::Pixel, Fallback);
        EXPECT_EQ(Value.Deriv, FMaterialCompiler::EDerivState::Valid) << Fallback;
        EXPECT_EQ(Value.DDX, FString("UV0_DDX")) << Fallback;
        EXPECT_EQ(Value.DDY, FString("UV0_DDY")) << Fallback;
    }
}

TEST(MaterialFallbackGradient, PositionAndColorFallbacksCarryTheirOwnGradients)
{
    FFallbackFixture F;
    for (const char* Fallback : { "WorldPosition", "WorldPositionExcludingOffsets", "VertexColor" })
    {
        const FMaterialCompiler::FInputValue Value = F.Resolve(EMaterialCompileStage::Pixel, Fallback);
        EXPECT_EQ(Value.Deriv, FMaterialCompiler::EDerivState::Valid) << Fallback;
        EXPECT_EQ(Value.DDX, FString(Fallback) + "_DDX");
        EXPECT_EQ(Value.DDY, FString(Fallback) + "_DDY");
    }
}

TEST(MaterialFallbackGradient, NormalFallbackIsUnknownRatherThanConstant)
{
    FFallbackFixture F;
    EXPECT_EQ(F.Resolve(EMaterialCompileStage::Pixel, "WorldNormal").Deriv, FMaterialCompiler::EDerivState::Unknown);
}

TEST(MaterialFallbackGradient, LiteralFallbacksStayConstant)
{
    FFallbackFixture F;
    EXPECT_EQ(F.Resolve(EMaterialCompileStage::Pixel, "0.5").Deriv, FMaterialCompiler::EDerivState::Zero);
    EXPECT_EQ(F.Resolve(EMaterialCompileStage::Pixel, "float2(1.0, 1.0)").Deriv, FMaterialCompiler::EDerivState::Zero);
    EXPECT_EQ(F.Resolve(EMaterialCompileStage::Pixel, "GetTime()").Deriv, FMaterialCompiler::EDerivState::Zero);
}

// No vertex template declares the _DDX pairs, so naming one there would not compile.
TEST(MaterialFallbackGradient, VertexStageNeverNamesAGradientPair)
{
    FFallbackFixture F;
    const FMaterialCompiler::FInputValue Value = F.Resolve(EMaterialCompileStage::Vertex, "float2(UV0)");
    EXPECT_NE(Value.Deriv, FMaterialCompiler::EDerivState::Valid);
    EXPECT_TRUE(Value.DDX.IsEmpty());
}
