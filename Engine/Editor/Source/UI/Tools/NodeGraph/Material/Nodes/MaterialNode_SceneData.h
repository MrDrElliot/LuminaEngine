#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_SceneData.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_ScreenPosition : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "ScreenPosition"; }
        FStringView GetNodeTooltip() const override { return "Returns the fragment's screen position. Normalized [0,1] by default; toggle Raw for pixels."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        PROPERTY(Editable) bool bRaw = false;
    };

    REFLECT()
    class CMaterialExpression_ViewDirection : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "ViewDirection"; }
        FStringView GetNodeTooltip() const override { return "Returns the unit vector from the fragment toward the camera (float3)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ReflectionVector : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "ReflectionVector"; }
        FStringView GetNodeTooltip() const override { return "Returns the world-space reflection vector of the view direction about the surface normal."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_FragmentDepth : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "FragmentDepth"; }
        FStringView GetNodeTooltip() const override { return "Returns fragment depth. Linear by default (view-space Z), toggle for raw NDC z."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        PROPERTY(Editable) bool bLinear = true;
    };

    REFLECT()
    class CMaterialExpression_ViewportSize : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "ViewportSize"; }
        FStringView GetNodeTooltip() const override { return "Returns the current viewport size in pixels (float2)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_AspectRatio : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "AspectRatio"; }
        FStringView GetNodeTooltip() const override { return "Returns viewport width / height."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_SceneColor : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "SceneColor"; }
        FStringView GetNodeTooltip() const override { return "Samples the scene color render target. Only valid in PostProcess materials. Optional UV input defaults to the screen UV."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_SceneDepth : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "SceneDepth"; }
        FStringView GetNodeTooltip() const override { return "Samples the scene depth attachment. Linear by default (view-space distance in world units), toggle for raw [0,1] NDC z. Only valid in PostProcess materials."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** When true, returns linear view-space distance; otherwise raw NDC z. */
        PROPERTY(Editable) bool bLinear = true;

        CMaterialInput* UV = nullptr;
    };

    REFLECT()
    class CMaterialExpression_SceneHDRColor : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "SceneHDRColor"; }
        FStringView GetNodeTooltip() const override { return "Samples the pre-tone-mapping HDR scene color. Useful for custom tonemapping or HDR-aware effects. Only valid in PostProcess materials."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* UV = nullptr;
    };
}
