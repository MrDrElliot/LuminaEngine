#include <gtest/gtest.h>

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Triplanar.h"

using namespace Lumina;

namespace
{
    struct FTriplanarFixture
    {
        FMaterialCompiler Compiler;
        CMaterialExpression_TriplanarSample* Node = nullptr;
        FMaterialCompiler::FTriplanarInputs Inputs;

        FTriplanarFixture()
        {
            Node = NewObject<CMaterialExpression_TriplanarSample>();
            Node->BuildNode();
            Inputs.TextureHandle = Node->TextureHandle;
            Inputs.Position  = Node->Position;
            Inputs.Normal    = Node->Normal;
            Inputs.Tiling    = Node->Tiling;
            Inputs.Sharpness = Node->Sharpness;
        }

        FString Emit(EMaterialCompileStage Stage, int32 TextureIndex, bool bNormalMap)
        {
            Compiler.SetStage(Stage);
            Compiler.TriplanarSample("Tri", Node, TextureIndex, Inputs, "SAMPLER_LINEAR_WRAP", bNormalMap);
            return Compiler.GetActiveChunk();
        }
    };

    bool Contains(const FString& Haystack, const char* Needle)
    {
        return Haystack.find(Needle) != FString::npos;
    }
}

// Pixel lanes must carry world position's own gradients, or the deferred lane picks mips from UV0.
TEST(MaterialTriplanar, PixelStageSamplesWithWorldPositionGradients)
{
    FTriplanarFixture F;
    const FString Code = F.Emit(EMaterialCompileStage::Pixel, 3, false);

    EXPECT_TRUE(Contains(Code, "float4 Tri = TriplanarSample(GetMaterialTexture(MaterialIndex, 3), SAMPLER_LINEAR_WRAP"));
    EXPECT_TRUE(Contains(Code, "WorldPosition_DDX, WorldPosition_DDY"));
    EXPECT_FALSE(F.Compiler.HasErrors());
    EXPECT_TRUE(F.Compiler.GetWarnings().empty());
}

// No vertex template declares *_DDX companions, so referencing one would not compile.
TEST(MaterialTriplanar, VertexStagePassesZeroGradients)
{
    FTriplanarFixture F;
    const FString Code = F.Emit(EMaterialCompileStage::Vertex, 0, false);

    EXPECT_FALSE(Contains(Code, "_DDX"));
    EXPECT_TRUE(Contains(Code, "float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)"));
    EXPECT_FALSE(F.Compiler.HasErrors());
}

TEST(MaterialTriplanar, NormalMapModeCallsTheTangentSpaceHelper)
{
    FTriplanarFixture F;
    const FString Code = F.Emit(EMaterialCompileStage::Pixel, 0, true);

    EXPECT_TRUE(Contains(Code, "float4 Tri = TriplanarSampleNormal("));
    EXPECT_TRUE(Contains(Code, "WorldTangent"));
}

// Downstream nodes read Tri by name, so a rejection must still declare it.
TEST(MaterialTriplanar, MissingTextureDeclaresNeutralValueAndErrors)
{
    FTriplanarFixture F;
    const FString Code = F.Emit(EMaterialCompileStage::Pixel, -1, true);

    EXPECT_TRUE(Contains(Code, "float4 Tri = float4(0.5, 0.5, 1.0, 1.0);"));
    EXPECT_FALSE(Contains(Code, "TriplanarSampleNormal("));
    EXPECT_TRUE(F.Compiler.HasErrors());
}

TEST(MaterialTriplanar, RejectedInUIMaterials)
{
    FTriplanarFixture F;
    F.Compiler.SetMaterialType(EMaterialType::UI);
    const FString Code = F.Emit(EMaterialCompileStage::Pixel, 0, false);

    EXPECT_TRUE(Contains(Code, "float4 Tri = float4(0.0, 0.0, 0.0, 1.0);"));
    EXPECT_TRUE(F.Compiler.HasErrors());
}

// A material function's texture input arrives as a handle, so the wire must beat the asset slot.
TEST(MaterialTriplanar, ConnectedTextureHandleOverridesTheBoundIndex)
{
    FTriplanarFixture F;
    auto* Source = NewObject<CMaterialExpression_TriplanarSample>();
    Source->BuildNode();
    CMaterialOutput* HandleOut = static_cast<CMaterialOutput*>(Source->GetOutputPins()[0].Get());
    HandleOut->SetInputType(EMaterialInputType::TextureHandle);
    HandleOut->ResolvedVar = "WiredHandle";
    F.Node->TextureHandle->AddConnection(HandleOut);

    const FString Code = F.Emit(EMaterialCompileStage::Pixel, -1, false);

    EXPECT_TRUE(Contains(Code, "float4 Tri = TriplanarSample(WiredHandle, SAMPLER_LINEAR_WRAP"));
    EXPECT_FALSE(Contains(Code, "GetMaterialTexture"));
    EXPECT_FALSE(F.Compiler.HasErrors());
}
