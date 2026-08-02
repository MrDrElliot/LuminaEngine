#include "MaterialNode_Noise.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{

    void CMaterialExpression_Hash11::BuildNode() { Super::BuildNode(); X = MakeIn(this, "X"); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_Hash11::GenerateDefinition(FMaterialCompiler& C) { C.Hash11(X); }

    void CMaterialExpression_Hash21::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_Hash21::GenerateDefinition(FMaterialCompiler& C) { C.Hash21(UV); }

    void CMaterialExpression_Hash22::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float2); }
    void CMaterialExpression_Hash22::GenerateDefinition(FMaterialCompiler& C) { C.Hash22(UV); }

    void CMaterialExpression_Hash33::BuildNode() { Super::BuildNode(); P = MakeIn(this, "P"); P->SetInputType(EMaterialInputType::Float3); Output->SetInputType(EMaterialInputType::Float3); }
    void CMaterialExpression_Hash33::GenerateDefinition(FMaterialCompiler& C) { C.Hash33(P); }

    void CMaterialExpression_ValueNoise::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_ValueNoise::GenerateDefinition(FMaterialCompiler& C) { C.ValueNoise(UV); }

    void CMaterialExpression_GradientNoise::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_GradientNoise::GenerateDefinition(FMaterialCompiler& C) { C.GradientNoise(UV); }

    void CMaterialExpression_PerlinNoise::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_PerlinNoise::GenerateDefinition(FMaterialCompiler& C) { C.PerlinNoise(UV); }

    void CMaterialExpression_VoronoiNoise::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_VoronoiNoise::GenerateDefinition(FMaterialCompiler& C) { C.VoronoiNoise(UV); }

    void CMaterialExpression_SimpleNoise::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_SimpleNoise::GenerateDefinition(FMaterialCompiler& C) { C.SimpleNoise(UV); }

    void CMaterialExpression_Checkerboard::BuildNode() { Super::BuildNode(); UV = MakeIn(this, "UV"); UV->SetInputType(EMaterialInputType::Float2); Output->SetInputType(EMaterialInputType::Float); }
    void CMaterialExpression_Checkerboard::GenerateDefinition(FMaterialCompiler& C) { C.Checkerboard(UV); }
}
