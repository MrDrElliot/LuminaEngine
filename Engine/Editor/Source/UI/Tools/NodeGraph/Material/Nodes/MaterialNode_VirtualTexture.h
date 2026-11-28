#pragma once
#include "MaterialNodeExpression.h"
#include "Assets/AssetTypes/Textures/RuntimeVirtualTexture.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MaterialNode_VirtualTexture.generated.h"

namespace Lumina
{
    // Reads a runtime virtual texture by world position, such as the ground color under a grass blade or a trail painted where the player walked.
    REFLECT()
    class CMaterialExpression_VirtualTextureSample : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Textures"; }
        FStringView GetNodeDisplayName() const override { return "VirtualTextureSample"; }
        FStringView GetNodeTooltip() const override
        {
            return "Samples a Runtime Virtual Texture at a world position: the pixel's own, the object's pivot when "
                   "Sample At Object Origin is on, or whatever is wired into Position.\n\n"
                   "Valid is 1 inside the texture's window and fades to 0 at its edge, where RGBA eases to the "
                   "texture's clear color. Terrains write into one through Virtual Texture Outputs, and entities "
                   "through a Virtual Texture Writer component.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Virtual Texture")
        TStrongObjectPtr<CRuntimeVirtualTexture> Texture;

        // One value per instance from under its pivot, so a grass blade takes a single ground color from root to tip.
        PROPERTY(Editable, Category = "Virtual Texture")
        bool bSampleAtObjectOrigin = false;

        CMaterialInput*  Position = nullptr;
        CMaterialOutput* RGBAOut  = nullptr;
        CMaterialOutput* RGBOut   = nullptr;
        CMaterialOutput* AlphaOut = nullptr;
        CMaterialOutput* ValidOut = nullptr;
    };
}
