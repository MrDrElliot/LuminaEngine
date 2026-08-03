#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Inputs.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_TexCoords : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "TexCoords"; }
        FStringView GetNodeTooltip() const override { return "Returns the mesh's UV coordinates from the given texcoord set, scaled by the tiling factors. Connect Tiling to drive the scale from another node; a scalar tiles both axes equally. Otherwise the UTiling/VTiling defaults are used."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        // Tiling multiplier. A scalar applies to both axes; wider than float2 is truncated to xy.
        CMaterialInput* Tiling = nullptr;

        /** Index of the UV set to sample from the mesh. */
        PROPERTY(Editable)
        uint32 TextureIndex = 0;

        /** Default tiling multiplier applied to the U axis when the Tiling pin is unconnected. */
        PROPERTY(Editable)
        float UTiling = 1.0f;

        /** Default tiling multiplier applied to the V axis when the Tiling pin is unconnected. */
        PROPERTY(Editable)
        float VTiling = 1.0f;
    };

    REFLECT()
    class CMaterialExpression_Panner : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "Panner"; }
        FStringView GetNodeTooltip() const override { return "Offsets UV coordinates over time. UV += Time * Speed."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
        CMaterialInput* Time = nullptr;
        CMaterialInput* Speed = nullptr;

        PROPERTY(Editable) 
        float SpeedX = 1.0f;
        
        PROPERTY(Editable) 
        float SpeedY = 1.0f;
    };

    REFLECT()
    class CMaterialExpression_WorldPos : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "WorldPosition"; }
        FStringView GetNodeTooltip() const override { return "Returns the current fragment's world-space position (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_CameraPos : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "CameraPosition"; }
        FStringView GetNodeTooltip() const override { return "Returns the world-space position of the active camera (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ObjectScale : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "ObjectScale"; }
        FStringView GetNodeTooltip() const override { return "Returns the world-space scale of the object being rendered (float3). Useful for keeping UV/texel density constant under non-uniform scaling."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ObjectPosition : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "ObjectPosition"; }
        FStringView GetNodeTooltip() const override { return "Returns the world-space position of the object's origin (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_EntityID : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "EntityID"; }
        FStringView GetNodeTooltip() const override { return "Returns the ID of the entity being rendered. Useful for per-entity effects."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_VertexNormal : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "VertexNormal"; }
        FStringView GetNodeTooltip() const override { return "Returns the interpolated world-space vertex normal (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_VertexTangent : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "VertexTangent"; }
        FStringView GetNodeTooltip() const override { return "Returns the interpolated world-space vertex tangent (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_VertexBitangent : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "VertexBitangent"; }
        FStringView GetNodeTooltip() const override { return "Returns the world-space vertex bitangent (float3, derived from normal x tangent * sign)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_VertexColor : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "VertexColor"; }
        FStringView GetNodeTooltip() const override { return "Returns the interpolated per-vertex color (float4)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };
}
