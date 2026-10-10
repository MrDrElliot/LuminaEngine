#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_TextureSample.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "MaterialNode_Triplanar.generated.h"

namespace Lumina
{
    REFLECT()
    class EDITOR_API CMaterialExpression_TriplanarSample : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Textures"; }
        void* GetNodeDefaultValue() override { return &Texture; }
        FName* GetParameterName() override { return &ParameterName; }
        FStringView GetNodeDisplayName() const override { return "TriplanarSample"; }
        FStringView GetNodeTooltip() const override
        {
            return "Samples a texture projected along the three world axes and blends by the surface normal, so it "
                   "needs no UVs and never stretches on steep faces. Normal Map mode outputs a tangent-space "
                   "normal ready for the Normal pin. Wire a TextureHandle into Texture to override the asset.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void SetNodeValue(void* Value) override;
        void DrawNodeBody() override;
        void DrawContextMenu() override;

        /** The texture projected onto each axis. Ignored while the Texture pin is connected. */
        PROPERTY(Editable, Category = "Texture")
        TStrongObjectPtr<CTexture> Texture;

        /** Name used to expose this texture as a material parameter for instancing (only used when bDynamic). */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        /** Filtering + address mode. Wrap is what a tiling projection wants. */
        PROPERTY(Editable, Category = "Texture")
        EMaterialSampler Sampler = EMaterialSampler::FromTexture;

        /** Treat the texture as a tangent-space normal map: blend per-plane normals and output one for the Normal pin. */
        PROPERTY(Editable, Category = "Texture")
        bool bNormalMap = false;

        CMaterialInput* TextureHandle = nullptr;
        CMaterialInput* Position  = nullptr;
        CMaterialInput* Normal    = nullptr;
        CMaterialInput* Tiling    = nullptr;
        CMaterialInput* Sharpness = nullptr;
    };
}
