#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "MaterialNode_TextureHandle.generated.h"

namespace Lumina
{
    /**
     * Binds a texture to a material slot and hands out its BINDLESS INDEX instead of sampling it.
     *
     * TextureSample answers "what color is this texture at this UV"; this node answers "which texture",
     * and leaves the sampling to you. The output is a uint you feed to the GlobalRHI.slang helpers from a
     * Custom Slang node:
     *
     *     float4 C = SampleTexture2D(MyTex, SAMPLER_LINEAR_WRAP, UV);
     *     float4 D = SampleTexture2DLevel(MyTex, SAMPLER_POINT_CLAMP, UV, 3.0);
     *     float4 E = SampleTexture2DArray(MyTex, SAMPLER_LINEAR_WRAP, UV, Slice);
     *
     * That is the point: a ray-march, a blur kernel or a manual mip walk needs the same texture at many
     * UVs and LODs, which a graph of TextureSample nodes cannot express without one node per tap.
     *
     * Because nothing is sampled here, the node does not care what VIEW the texture has -- a CTextureArray
     * works exactly as well as a plain 2D texture, since the bindless heap aliases gTextures2D,
     * gTextures2DArray and gTexturesCube at the same binding. Picking the matching sample helper is
     * yours to get right; a mismatch reads the null descriptor rather than failing to compile.
     *
     * The emitted value is the slot's RUNTIME contents, not a compile-time constant, so a texture
     * parameter (bDynamic + ParameterName) can still be swapped per material instance without a recompile.
     *
     * The handle is a uint, not a float, and the material graph schema enforces that: it will only
     * connect to a pin that declared itself a texture handle. Wiring an index into float math would
     * compile -- uint promotes silently -- and produce nonsense, so the wire is refused instead.
     */
    REFLECT()
    class CMaterialExpression_TextureHandle : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Textures"; }
        void* GetNodeDefaultValue() override { return &Texture; }
        FName* GetParameterName() override { return &ParameterName; }
        FStringView GetNodeDisplayName() const override { return "TextureHandle"; }
        FStringView GetNodeTooltip() const override
        {
            return "The texture's bindless index, for sampling it yourself in a Custom Slang node.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void SetNodeValue(void* Value) override;
        void DrawNodeBody() override;
        void DrawContextMenu() override;

        /** The texture whose slot index this node resolves. Any view type; nothing is sampled here. */
        PROPERTY(Editable, Category = "Texture")
        TObjectPtr<CTexture> Texture;

        /** Name used to expose this texture as a material parameter for instancing (only used when bDynamic). */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;
    };
}
