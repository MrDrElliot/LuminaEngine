#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Color.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_Luminance : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Luminance"; }
        FStringView GetNodeTooltip() const override { return "Returns the perceived brightness of an RGB color (Rec. 709 luma)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Desaturate : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Desaturate"; }
        FStringView GetNodeTooltip() const override { return "Blends an RGB color toward grayscale by Amount [0,1]."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* Amount = nullptr;
    };

    REFLECT()
    class CMaterialExpression_RGBToHSV : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "RGBToHSV"; }
        FStringView GetNodeTooltip() const override { return "Converts an RGB color to HSV (Hue/Saturation/Value)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* RGB = nullptr;
    };

    REFLECT()
    class CMaterialExpression_HSVToRGB : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "HSVToRGB"; }
        FStringView GetNodeTooltip() const override { return "Converts an HSV color back to RGB."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* HSV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Posterize : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Posterize"; }
        FStringView GetNodeTooltip() const override { return "Quantizes color into N discrete bands."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* Steps = nullptr;
    };

    REFLECT()
    class CMaterialExpression_GammaCorrection : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "GammaCorrect"; }
        FStringView GetNodeTooltip() const override { return "Applies pow(Color, Gamma) to the input."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* Gamma = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Contrast : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Contrast"; }
        FStringView GetNodeTooltip() const override { return "Stretches values around 0.5 by Amount."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* Amount = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Brightness : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Brightness"; }
        FStringView GetNodeTooltip() const override { return "Multiplies color by Amount."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* Amount = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Tint : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "Tint"; }
        FStringView GetNodeTooltip() const override { return "Modulates Color by TintColor blended in by Amount."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
        CMaterialInput* TintColor = nullptr;
        CMaterialInput* Amount = nullptr;
    };

    REFLECT()
    class CMaterialExpression_LinearToSRGB : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "LinearToSRGB"; }
        FStringView GetNodeTooltip() const override { return "Approximate linear -> sRGB conversion (gamma 1/2.2)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
    };

    REFLECT()
    class CMaterialExpression_SRGBToLinear : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Color"; }
        FStringView GetNodeDisplayName() const override { return "SRGBToLinear"; }
        FStringView GetNodeTooltip() const override { return "Approximate sRGB -> linear conversion (gamma 2.2)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* Color = nullptr;
    };
}
