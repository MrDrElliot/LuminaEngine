#include "MaterialNode_Surface.h"

#include "MaterialNodePinHelpers.h"
#include "MaterialOutputNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    namespace
    {
        constexpr const char* kDefaultSurface = "DefaultMaterialInputs()";
        constexpr float kDefaultHeightContrast = 8.0f;

        void MakeSurfaceOutput(CMaterialOutput* Output)
        {
            Output->SetPinName("Surface");
            Output->SetHideDuringConnection(false);
            Output->SetInputType(EMaterialInputType::Surface);
            Output->SetComponentMask(EComponentMask::None);
        }
    }

    void CMaterialExpression_MakeSurface::BuildNode()
    {
        Super::BuildNode();
        MakeSurfaceOutput(Output);

        FieldPins.Reset();
        for (const FMaterialSurfaceField& Field : kMaterialSurfaceFields)
        {
            FieldPins.Add(MakeIn(this, Field.Name, Field.Width == 3 ? EMaterialInputType::Float3 : EMaterialInputType::Float));
        }
    }

    void CMaterialExpression_MakeSurface::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        FString Code = "FMaterialPixelInputs " + FullName + " = " + kDefaultSurface + ";\n";
        bool bExtendedLobes = false;
        for (int32 i = 0; i < FieldPins.Num(); ++i)
        {
            if (!CMaterialOutputNode::IsPinDriven(FieldPins[i]))
            {
                continue;
            }
            const FMaterialSurfaceField& Field = kMaterialSurfaceFields[i];
            Code += CMaterialOutputNode::EmitFieldAssignment(FullName, Field.Member, FieldPins[i], Field.Default, Field.Width);
            if (Field.bNormalMap)
            {
                Code += CMaterialOutputNode::EmitNormalDecode(FullName, Field.Member);
            }
            bExtendedLobes |= Field.bExtendedLobe;
        }
        Compiler.AddRaw(Code);

        if (bExtendedLobes)
        {
            Compiler.SetExtendedSurfaceDriven(true);
        }
    }

    void CMaterialExpression_BlendSurfaces::BuildNode()
    {
        Super::BuildNode();
        MakeSurfaceOutput(Output);
        A     = MakeIn(this, "A", EMaterialInputType::Surface);
        B     = MakeIn(this, "B", EMaterialInputType::Surface);
        Alpha = MakeIn(this, "Alpha");
    }

    void CMaterialExpression_BlendSurfaces::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        const FString AValue     = Compiler.GetTypedInputValue(A, kDefaultSurface).Value;
        const FString BValue     = Compiler.GetTypedInputValue(B, kDefaultSurface).Value;
        const FString AlphaValue = FMaterialCompiler::ValueAsWidth(Compiler.GetTypedInputValue(Alpha, 0.5f), 1);
        Compiler.AddRaw("FMaterialPixelInputs " + FullName + " = BlendMaterialSurfaces(" + AValue + ", " + BValue + ", " + AlphaValue + ");\n");
    }

    void CMaterialExpression_LayerSurfaces::BuildNode()
    {
        Super::BuildNode();
        MakeSurfaceOutput(Output);
        Bottom         = MakeIn(this, "Bottom", EMaterialInputType::Surface);
        Top            = MakeIn(this, "Top", EMaterialInputType::Surface);
        Coverage       = MakeIn(this, "Coverage");
        Height         = MakeIn(this, "Height");
        HeightContrast = MakeIn(this, "Height Contrast");
    }

    void CMaterialExpression_LayerSurfaces::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        const FString BottomValue   = Compiler.GetTypedInputValue(Bottom, kDefaultSurface).Value;
        const FString TopValue      = Compiler.GetTypedInputValue(Top, kDefaultSurface).Value;
        const FString CoverageValue = FMaterialCompiler::ValueAsWidth(Compiler.GetTypedInputValue(Coverage, 0.5f), 1);
        const FString HeightValue   = FMaterialCompiler::ValueAsWidth(Compiler.GetTypedInputValue(Height, 0.5f), 1);
        // Zero contrast reduces the height test to Coverage alone, which is what an unconnected Height should mean.
        const FString ContrastValue = CMaterialOutputNode::IsPinDriven(Height)
            ? FMaterialCompiler::ValueAsWidth(Compiler.GetTypedInputValue(HeightContrast, kDefaultHeightContrast), 1)
            : FString("0.0");
        Compiler.AddRaw("FMaterialPixelInputs " + FullName + " = LayerMaterialSurfaces(" + BottomValue + ", " + TopValue + ", "
            + CoverageValue + ", " + HeightValue + ", " + ContrastValue + ");\n");
    }
}
