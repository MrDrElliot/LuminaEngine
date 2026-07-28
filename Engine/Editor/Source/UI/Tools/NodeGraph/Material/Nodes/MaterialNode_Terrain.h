#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Terrain.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_TerrainLayerWeight : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(55, 125, 55, 255); }
        FFixedString GetNodeCategory() const override { return "Terrain"; }
        FStringView GetNodeDisplayName() const override { return "TerrainLayerWeight"; }
        FStringView GetNodeTooltip() const override { return "Returns the painted weight for the specified terrain layer (0-3) at the current fragment."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Layer") uint32 LayerIndex = 0;
    };

    REFLECT()
    class CMaterialExpression_TerrainLayerWeights : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(55, 125, 55, 255); }
        FFixedString GetNodeCategory() const override { return "Terrain"; }
        FStringView GetNodeDisplayName() const override { return "TerrainLayerWeights"; }
        FStringView GetNodeTooltip() const override { return "Returns all four terrain layer weights as a float4."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_TerrainLayerBlend : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(55, 125, 55, 255); }
        FFixedString GetNodeCategory() const override { return "Terrain"; }
        FStringView GetNodeDisplayName() const override { return "TerrainLayerBlend"; }
        FStringView GetNodeTooltip() const override { return "Blends up to 4 per-layer float3 inputs by the painted layer weights."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Layer0 = nullptr;
        CMaterialInput* Layer1 = nullptr;
        CMaterialInput* Layer2 = nullptr;
        CMaterialInput* Layer3 = nullptr;
    };
}
