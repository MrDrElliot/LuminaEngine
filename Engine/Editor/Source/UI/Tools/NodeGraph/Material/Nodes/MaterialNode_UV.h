#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_UV.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_RotateUV : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        FStringView GetNodeDisplayName() const override { return "RotateUV"; }
        FStringView GetNodeTooltip() const override { return "Rotates UV around Center by Rotation (radians)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* Center = nullptr;
        CMaterialInput* Rotation = nullptr;
    };

    REFLECT()
    class CMaterialExpression_TilingAndOffset : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        FStringView GetNodeDisplayName() const override { return "TilingAndOffset"; }
        FStringView GetNodeTooltip() const override { return "Returns UV * Tiling + Offset."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* Tiling = nullptr;
        CMaterialInput* Offset = nullptr;
    };

    REFLECT()
    class CMaterialExpression_FlipBook : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        FStringView GetNodeDisplayName() const override { return "FlipBook"; }
        FStringView GetNodeTooltip() const override { return "Animated sprite-sheet UV, slices the input UV into NumColumns x NumRows tiles, advances at FPS frames per second."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* NumColumns = nullptr;
        CMaterialInput* NumRows = nullptr;
        CMaterialInput* Time = nullptr;
        CMaterialInput* FPS = nullptr;
    };

    REFLECT()
    class CMaterialExpression_PolarCoordinates : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        FStringView GetNodeDisplayName() const override { return "PolarCoordinates"; }
        FStringView GetNodeTooltip() const override { return "Converts UV to polar coordinates (radius, angle) around Center."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* Center = nullptr;
    };

    REFLECT()
    class CMaterialExpression_TwirlUV : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        FStringView GetNodeDisplayName() const override { return "TwirlUV"; }
        FStringView GetNodeTooltip() const override { return "Twists UV around Center by Strength * radius."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* Center = nullptr;
        CMaterialInput* Strength = nullptr;
    };
}
