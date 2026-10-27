#pragma once
#include "MaterialNodeExpression.h"
#include "Containers/Span.h"
#include "MaterialNode_Formula.generated.h"

namespace Lumina
{
    struct FMaterialFormulaPin
    {
        const char* Name;

        // Components the code reads the pin as, or 0 for the width shared by every such pin.
        int32       Width    = 1;
        float       Default  = 0.0f;

        // A shader value read while nothing is wired, in place of the editable Default.
        const char* Fallback = nullptr;
    };

    enum class EMaterialFormulaLimit : uint8
    {
        None,
        NoUI,
        PixelOnly,
    };

    // Code is Slang in which $ names the node, {T} is the shared width's type and {Pin} reads that input.
    struct FMaterialFormula
    {
        FStringView                      Name;
        FStringView                      Category;
        FStringView                      Tooltip;
        TSpan<const FMaterialFormulaPin> Inputs;

        // Components of the single result, or 0 for the shared width.
        int32                            Width = 1;
        FStringView                      Code;

        // Named results, each declared by the code as $_Name, in place of the single result.
        TSpan<const FMaterialFormulaPin> Outputs = {};
        EMaterialFormulaLimit            Limit   = EMaterialFormulaLimit::None;
    };

    // A node defined entirely by its formula, so adding one needs no compiler code.
    REFLECT(NotPlaceable)
    class CMaterialExpression_Formula : public CMaterialExpression
    {
        GENERATED_BODY()

    public:

        virtual const FMaterialFormula& GetFormula() const;

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return GetFormula().Category.data(); }
        FStringView GetNodeDisplayName() const override { return GetFormula().Name; }
        FStringView GetNodeTooltip() const override { return GetFormula().Tooltip; }
        void* GetNodeDefaultValue() override { return Values.data(); }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        // Each unwired input's value, by pin index.
        PROPERTY()
        TVector<float> Values;
    };

    REFLECT()
    class CMaterialExpression_InverseLerp : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RemapClamped : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SmoothMin : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SmoothMax : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ConstantBiasScale : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Snap : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Wrap : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_PingPong : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Lerp3 : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DistanceSquared : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Bias : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Gain : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_EaseIn : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_EaseOut : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_EaseInOut : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SineWave : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SquareWave : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_TriangleWave : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SawtoothWave : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_PulseWave : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Spherize : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RadialShear : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Pixelate : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_MirrorUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_FlipUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ScaleUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BumpOffset : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_WorldAlignedUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_GridCells : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_LensDistortion : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_WavyUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DistortUV : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Circle : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Rectangle : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RoundedRectangle : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Polygon : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Ring : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Star : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_LineSegment : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_PlusShape : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_LinearGradient : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RadialGradient : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_AngularGradient : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DiamondGradient : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Stripes : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_GridLines : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Bricks : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_HexGrid : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Dots : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Spiral : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Vignette : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_FBMNoise : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RidgedNoise : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_TurbulenceNoise : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_FBMNoise3D : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SimplexNoise : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_VoronoiCells : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DomainWarp : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_RandomRange : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ScreenDither : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_HueShift : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Saturation : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Vibrance : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Exposure : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Levels : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_WhiteBalance : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_CosinePalette : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Blackbody : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ChannelMixer : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ReplaceColor : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_ColorMask : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_CheapContrast : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Sepia : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_HueToRGB : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendMultiply : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendScreen : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendOverlay : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendSoftLight : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendHardLight : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendColorDodge : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendColorBurn : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendLinearDodge : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendLinearBurn : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendLinearLight : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendVividLight : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendPinLight : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendHardMix : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendDifference : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendExclusion : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendSubtract : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendDivide : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendDarken : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_BlendLighten : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SphereMask : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_CameraDistance : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DistanceFade : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_CameraVectors : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SlopeMask : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_HeightLerp : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_NormalStrength : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_FlattenNormal : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_TangentToWorld : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_WorldToTangent : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_SoftThreshold : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_InRange : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Switch : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_DitherFade : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };

    REFLECT()
    class CMaterialExpression_Wobble : public CMaterialExpression_Formula
    {
        GENERATED_BODY()
        const FMaterialFormula& GetFormula() const override;
    };
}
