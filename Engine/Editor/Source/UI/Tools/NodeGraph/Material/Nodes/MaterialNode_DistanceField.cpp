#include "MaterialNode_DistanceField.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_MeshDistanceField::BuildNode()
    {
        Position = MakeIn(this, "Position", EMaterialInputType::Float3);

        DistanceOut = MakeOut(this, "Distance", EMaterialInputType::Float);
        GradientOut = MakeOut(this, "Gradient", EMaterialInputType::Float3);
        ValidOut    = MakeOut(this, "Valid",    EMaterialInputType::Float);
    }

    void CMaterialExpression_MeshDistanceField::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.MeshDistanceField(this, Position, DistanceOut, GradientOut, ValidOut);
    }

    void CMaterialExpression_MeshDistanceFieldOcclusion::BuildNode()
    {
        Normal    = MakeIn(this, "Normal", EMaterialInputType::Float3);
        Radius    = MakeIn(this, "Radius");
        ConeAngle = MakeIn(this, "Cone Angle");
        Intensity = MakeIn(this, "Intensity");

        OcclusionOut = MakeOut(this, "Occlusion", EMaterialInputType::Float);
    }

    void CMaterialExpression_MeshDistanceFieldOcclusion::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        FMaterialCompiler::FDistanceFieldOcclusionInputs Inputs;
        Inputs.Normal    = Normal;
        Inputs.Radius    = Radius;
        Inputs.ConeAngle = ConeAngle;
        Inputs.Intensity = Intensity;

        Compiler.MeshDistanceFieldOcclusion(this, Inputs, Math::Clamp(Steps, 2, 64), OcclusionOut);
    }

    void CMaterialExpression_MeshDistanceFieldThickness::BuildNode()
    {
        Normal      = MakeIn(this, "Normal", EMaterialInputType::Float3);
        MaxDistance = MakeIn(this, "Max Distance");

        ThicknessOut  = MakeOut(this, "Thickness",  EMaterialInputType::Float);
        NormalizedOut = MakeOut(this, "Normalized", EMaterialInputType::Float);
    }

    void CMaterialExpression_MeshDistanceFieldThickness::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.MeshDistanceFieldThickness(this, Normal, MaxDistance, Math::Clamp(Steps, 2, 64),
                                            ThicknessOut, NormalizedOut);
    }
}
