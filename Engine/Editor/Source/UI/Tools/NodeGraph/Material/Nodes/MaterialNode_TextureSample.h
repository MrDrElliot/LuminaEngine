#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "MaterialNode_TextureSample.generated.h"

namespace Lumina
{
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

        CMaterialInput* UV = nullptr;
    };
}
