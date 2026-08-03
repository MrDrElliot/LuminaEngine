#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "MaterialNode_Parallax.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_ParallaxOcclusionMapping : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "UV"; }
        void* GetNodeDefaultValue() override { return &HeightMap; }
        FName* GetParameterName() override { return &ParameterName; }
        FStringView GetNodeDisplayName() const override { return "ParallaxOcclusionMapping"; }
        FStringView GetNodeTooltip() const override
        {
            return "Ray-traces a height map in tangent space and returns the displaced UV. Feed that UV to every "
                   "texture sample on the surface. The height field is pushed DOWN from the polygon: white is the "
                   "top, black the deepest. Silhouettes stay flat.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void SetNodeValue(void* Value) override;
        void DrawNodeBody() override;

        /** Height field. Grayscale, read from the red channel; white = polygon surface, black = deepest. */
        PROPERTY(Editable, Category = "Texture")
        TObjectPtr<CTexture> HeightMap;

        /** Name used to expose the height map as a material parameter for instancing (only used when bDynamic). */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        CMaterialInput* UV             = nullptr;
        CMaterialInput* HeightScale    = nullptr;
        CMaterialInput* MinSamples     = nullptr;
        CMaterialInput* MaxSamples     = nullptr;
        CMaterialInput* LODThreshold   = nullptr;
        CMaterialInput* ShadowSamples  = nullptr;
        CMaterialInput* ShadowSoftness = nullptr;

        CMaterialOutput* UVOut     = nullptr;
        CMaterialOutput* ShadowOut = nullptr;
        CMaterialOutput* HeightOut = nullptr;
    };
}
