#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "MaterialNode_TextureSampleArray.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_TextureSampleArray : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Textures"; }
        void* GetNodeDefaultValue() override { return &TextureArray; }
        FName* GetParameterName() override { return &ParameterName; }
        FStringView GetNodeDisplayName() const override { return "TextureSampleArray"; }
        FStringView GetNodeTooltip() const override
        {
            return "Samples one layer of a Texture Array. Slice picks the layer and is NOT blended between "
                   "layers -- the hardware treats it as a discrete index, which is the point, since layers "
                   "are unrelated images. It is rounded and clamped to the array's range.\n\n"
                   "Use it for per-instance variety (a single material driving many leaf, decal or debris "
                   "textures) by feeding Slice from EntityID, a custom primitive data value, or a hash of "
                   "object position. That keeps one draw call where separate textures would need one "
                   "material per variant.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        void SetNodeValue(void* Value) override;
        void DrawNodeBody() override;

        /** The texture array asset to sample. */
        PROPERTY(Editable, Category = "Texture")
        TObjectPtr<CTextureArray> TextureArray;

        /** Name used to expose this array as a material parameter for instancing (only used when bDynamic). */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        CMaterialInput* UV    = nullptr;
        CMaterialInput* Slice = nullptr;
    };
}
