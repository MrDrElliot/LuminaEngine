#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Noise.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_Hash11 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Hash11"; }
        FStringView GetNodeTooltip() const override { return "Cheap 1D -> 1D pseudo-random hash."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* X = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Hash21 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Hash21"; }
        FStringView GetNodeTooltip() const override { return "Cheap 2D -> 1D pseudo-random hash."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Hash22 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Hash22"; }
        FStringView GetNodeTooltip() const override { return "Cheap 2D -> 2D pseudo-random hash."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Hash33 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Hash33"; }
        FStringView GetNodeTooltip() const override { return "Cheap 3D -> 3D pseudo-random hash."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* P = nullptr;
    };

    REFLECT()
    class CMaterialExpression_ValueNoise : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "ValueNoise"; }
        FStringView GetNodeTooltip() const override { return "Smooth-interpolated 2D value noise in [0,1]."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_GradientNoise : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "GradientNoise"; }
        FStringView GetNodeTooltip() const override { return "2D gradient noise in [0,1] (Perlin-like)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_PerlinNoise : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "PerlinNoise"; }
        FStringView GetNodeTooltip() const override { return "2D Perlin gradient noise in [0,1]."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_VoronoiNoise : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "VoronoiNoise"; }
        FStringView GetNodeTooltip() const override { return "2D Voronoi/Worley noise, distance to the closest cell point."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_SimpleNoise : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "SimpleNoise"; }
        FStringView GetNodeTooltip() const override { return "Cheapest pseudo-random noise, a single sin-hash. High-frequency sparkle."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Checkerboard : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Checkerboard"; }
        FStringView GetNodeTooltip() const override { return "Returns a 0/1 checkerboard pattern based on UV cells."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        CMaterialInput* UV = nullptr;
    };
}
