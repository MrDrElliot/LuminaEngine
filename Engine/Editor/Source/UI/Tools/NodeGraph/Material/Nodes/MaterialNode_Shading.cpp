#include "MaterialNode_Shading.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{

    void CMaterialExpression_Fresnel::BuildNode()
    {
        Super::BuildNode();
        Exponent    = MakeIn(this, "Exponent");
        BaseReflect = MakeIn(this, "BaseReflect");
        Normal      = MakeIn(this, "Normal");
        Output->SetInputType(EMaterialInputType::Float);
    }
    void CMaterialExpression_Fresnel::GenerateDefinition(FMaterialCompiler& C) { C.Fresnel(Exponent, BaseReflect, Normal); }

    void CMaterialExpression_DepthFade::BuildNode()
    {
        Super::BuildNode();
        FadeDistance = MakeIn(this, "FadeDistance");
        Output->SetInputType(EMaterialInputType::Float);
    }
    void CMaterialExpression_DepthFade::GenerateDefinition(FMaterialCompiler& C) { C.DepthFade(FadeDistance); }

    void CMaterialExpression_NormalFromHeight::BuildNode()
    {
        Super::BuildNode();
        Height   = MakeIn(this, "Height");
        Strength = MakeIn(this, "Strength");
        Output->SetInputType(EMaterialInputType::Float3);
    }
    void CMaterialExpression_NormalFromHeight::GenerateDefinition(FMaterialCompiler& C) { C.NormalFromHeight(Height, Strength); }

    void CMaterialExpression_DeriveNormalZ::BuildNode()
    {
        Super::BuildNode();
        InputXY = MakeIn(this, "XY");
        InputXY->SetInputType(EMaterialInputType::Float2);
        Output->SetInputType(EMaterialInputType::Float3);
    }
    void CMaterialExpression_DeriveNormalZ::GenerateDefinition(FMaterialCompiler& C) { C.DeriveNormalZ(InputXY); }

    void CMaterialExpression_BlendNormals::BuildNode()
    {
        Super::BuildNode();
        A = MakeIn(this, "A");
        B = MakeIn(this, "B");
        Output->SetInputType(EMaterialInputType::Float3);
    }
    void CMaterialExpression_BlendNormals::GenerateDefinition(FMaterialCompiler& C) { C.BlendNormals(A, B); }
}
