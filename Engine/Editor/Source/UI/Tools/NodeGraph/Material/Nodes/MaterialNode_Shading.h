#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Shading.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_Fresnel : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Shading"; }
        FStringView GetNodeDisplayName() const override { return "Fresnel"; }
        FStringView GetNodeTooltip() const override { return "Schlick fresnel: BaseReflect + (1 - BaseReflect) * (1 - NoV)^Exponent. Defaults to surface normal."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Exponent = nullptr;
        CMaterialInput* BaseReflect = nullptr;
        CMaterialInput* Normal = nullptr;
    };

    REFLECT()
    class CMaterialExpression_DepthFade : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Shading"; }
        FStringView GetNodeDisplayName() const override { return "DepthFade"; }
        FStringView GetNodeTooltip() const override { return "Returns saturate(viewDepth / FadeDistance), useful for distance-based fading."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* FadeDistance = nullptr;
    };

    REFLECT()
    class CMaterialExpression_NormalFromHeight : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Shading"; }
        FStringView GetNodeDisplayName() const override { return "NormalFromHeight"; }
        FStringView GetNodeTooltip() const override { return "Builds a tangent-space normal from a height field using ddx/ddy. Output is already decoded (no *2-1 needed)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Height = nullptr;
        CMaterialInput* Strength = nullptr;
    };

    REFLECT()
    class CMaterialExpression_DeriveNormalZ : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Shading"; }
        FStringView GetNodeDisplayName() const override { return "DeriveNormalZ"; }
        FStringView GetNodeTooltip() const override { return "Reconstructs Z for a tangent-space normal stored as XY (e.g. BC5). Decodes from [0,1] to [-1,1] internally."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* InputXY = nullptr;
    };

    REFLECT()
    class CMaterialExpression_BlendNormals : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Shading"; }
        FStringView GetNodeDisplayName() const override { return "BlendNormals"; }
        FStringView GetNodeTooltip() const override { return "Reoriented Normal Mapping (RNM) blend of two tangent-space normals. Input/output stored in [0,1]."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* A = nullptr;
        CMaterialInput* B = nullptr;
    };
}
