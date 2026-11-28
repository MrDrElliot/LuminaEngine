#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Surface.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_MakeSurface : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Surface"; }
        FStringView GetNodeDisplayName() const override { return "Make Surface"; }
        FStringView GetNodeTooltip() const override
        {
            return "Packs every surface attribute into one Surface wire, for Blend Surfaces, Layer Surfaces or the "
                   "output's Surface pin. Unconnected attributes keep the output's defaults, and Normal takes a "
                   "normal map sample like the output's Normal pin.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        TVector<CMaterialInput*> FieldPins;
    };

    REFLECT()
    class CMaterialExpression_BlendSurfaces : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Surface"; }
        FStringView GetNodeDisplayName() const override { return "Blend Surfaces"; }
        FStringView GetNodeTooltip() const override
        {
            return "Mixes two surfaces attribute by attribute, A at Alpha 0 and B at 1, renormalizing the normal. "
                   "The result shades with the material's one shading model.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* A     = nullptr;
        CMaterialInput* B     = nullptr;
        CMaterialInput* Alpha = nullptr;
    };

    REFLECT()
    class CMaterialExpression_LayerSurfaces : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Surface"; }
        FStringView GetNodeDisplayName() const override { return "Layer Surfaces"; }
        FStringView GetNodeTooltip() const override
        {
            return "Lays Top over Bottom by Coverage. With Height connected, Top fills the high ground first and "
                   "Height Contrast sharpens its edge, as paint wearing off ridges or rust gathering in pits "
                   "(feed it an inverted height). Without Height it is a plain blend by Coverage.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Bottom         = nullptr;
        CMaterialInput* Top            = nullptr;
        CMaterialInput* Coverage       = nullptr;
        CMaterialInput* Height         = nullptr;
        CMaterialInput* HeightContrast = nullptr;
    };
}
