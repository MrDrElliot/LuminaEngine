#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "MaterialNode_TextureSample.generated.h"

namespace Lumina
{
    /**
     * Which stock sampler a texture sample uses. Values mirror RHI::EStockSampler and the SAMPLER_*
     * constants in GlobalRHI.slang -- all three must stay in lockstep, so the compiler can emit the
     * index as a literal.
     *
     * Baked into the compiled shader, not an instance parameter: the sample call takes the index as a
     * constant, and a per-instance value would need dynamic (non-uniform) sampler indexing.
     */
    REFLECT()
    enum class EMaterialSampler : uint8
    {
        LinearWrap   = 0,
        LinearClamp  = 1,
        LinearMirror = 2,
        PointWrap    = 3,
        PointClamp   = 4,
        AnisoWrap    = 5,
        AnisoClamp   = 6,
    };

    /** SAMPLER_* identifier for the emitted Slang. */
    FStringView MaterialSamplerToSlang(EMaterialSampler Sampler);

    REFLECT()
    class CMaterialExpression_TextureSample : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Textures"; }
        void* GetNodeDefaultValue() override { return &Texture; }
        FName* GetParameterName() override { return &ParameterName; }
        FStringView GetNodeDisplayName() const override { return "TextureSample"; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void SetNodeValue(void* Value) override;
        void DrawNodeBody() override;
        void DrawContextMenu() override;

        /** The texture asset to sample in this node. */
        PROPERTY(Editable, Category = "Texture")
        TObjectPtr<CTexture> Texture;

        /** Name used to expose this texture as a material parameter for instancing (only used when bDynamic). */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        /**
         * Filtering + address mode. Wrap tiles, Clamp holds the edge texel, Mirror reflects. Changing this
         * recompiles the material: the sampler index is a shader constant, not an instance parameter.
         */
        PROPERTY(Editable, Category = "Texture")
        EMaterialSampler Sampler = EMaterialSampler::LinearWrap;

        CMaterialInput* UV = nullptr;
    };
}
