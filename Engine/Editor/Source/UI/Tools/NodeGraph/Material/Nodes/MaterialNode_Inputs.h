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

        // Rotation in RADIANS about the UV origin, applied after tiling. Overrides RotationDegrees when
        // connected. A uniform (parameter or literal) keeps exact UV gradients; a per-pixel value cannot.
        CMaterialInput* Rotation = nullptr;

        /** Index of the UV set to sample from the mesh. */
        PROPERTY(Editable)
        uint32 TextureIndex = 0;

        /** Default tiling multiplier applied to the U axis when the Tiling pin is unconnected. */
        PROPERTY(Editable)
        float UTiling = 1.0f;

        /** Default tiling multiplier applied to the V axis when the Tiling pin is unconnected. */
        PROPERTY(Editable)
        float VTiling = 1.0f;

        /** Rotation about the UV origin, applied after tiling, when the Rotation pin is unconnected. */
        PROPERTY(Editable, Units = "deg")
        float RotationDegrees = 0.0f;
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

    // Which position the WorldPosition node reports, along both axes of the choice.
    REFLECT()
    enum class EWorldPositionType : uint8
    {
        Absolute,
        AbsoluteExcludingOffsets,
        CameraRelative,
        CameraRelativeExcludingOffsets,
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
        FStringView GetNodeTooltip() const override
        {
            return "World-space position of the point being shaded (float3).\n\n"
                   "Absolute is measured from the scene origin. CameraRelative subtracts the camera "
                   "position, which keeps the value small far from the origin and is what noise and "
                   "detail math should use there.\n\n"
                   "ExcludingOffsets reads the position the vertex had before World Position Offset "
                   "displaced it, so a pattern driven by it stays nailed to geometry that sways or "
                   "morphs instead of swimming across it. The two are the same value on a chain feeding "
                   "World Position Offset, which runs before the offset exists.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Which of the four world positions this node reports. */
        PROPERTY(Editable, Category = "World Position")
        EWorldPositionType PositionType = EWorldPositionType::Absolute;
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
    class CMaterialExpression_LocalBounds : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(25, 25, 255, 255); }
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "LocalBounds"; }
        FStringView GetNodeTooltip() const override
        {
            return "Object-space bounding box of the mesh being rendered, before any instance transform.\n\n"
                   "Min and Max are the box corners, HalfExtents is half its size and FullExtents the whole "
                   "size. Divide a local position by FullExtents to get a 0-1 gradient that holds its shape "
                   "across differently sized meshes.\n\n"
                   "Surface (PBR) materials only; anything else reads a unit box.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialOutput* HalfExtentsOut = nullptr;
        CMaterialOutput* FullExtentsOut = nullptr;
        CMaterialOutput* MinOut         = nullptr;
        CMaterialOutput* MaxOut         = nullptr;
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
        FStringView GetNodeDisplayName() const override { return "VertexNormalWS"; }
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
        FStringView GetNodeDisplayName() const override { return "VertexTangentWS"; }
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
        FStringView GetNodeDisplayName() const override { return "VertexBitangentWS"; }
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
