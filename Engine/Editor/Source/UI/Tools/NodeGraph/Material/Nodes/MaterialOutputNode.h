#pragma once

#include "MaterialGraphNode.h"
#include "MaterialOutputNode.generated.h"

namespace Lumina
{
    class CMaterialOutput;
    class FMaterialCompiler;

    struct FMaterialSurfaceField
    {
        const char* Name;
        const char* Member;
        const char* Default;
        int32       Width;
        // Driving it away from its default is what makes the material carry the extended lobes.
        bool        bExtendedLobe = false;
        // Takes a normal map sample, which is decoded to a unit tangent-space normal once assigned.
        bool        bNormalMap = false;
    };

    // Every FMaterialPixelInputs field a graph can drive, in the order GetPixelStagePins lists their output pins.
    inline constexpr FMaterialSurfaceField kMaterialSurfaceFields[] =
    {
        { "Base Color",              "Diffuse",               "float3(1.0, 1.0, 1.0)", 3 },
        { "Metallic",                "Metallic",              "0.0",                   1 },
        { "Roughness",               "Roughness",             "1.0",                   1 },
        { "Specular",                "Specular",              "0.5",                   1 },
        { "Emissive",                "Emissive",              "float3(0.0, 0.0, 0.0)", 3 },
        { "Ambient Occlusion",       "AmbientOcclusion",      "1.0",                   1 },
        { "Normal",                  "Normal",                "float3(0.0, 0.0, 1.0)", 3, false, true },
        { "Opacity",                 "Opacity",               "1.0",                   1 },
        { "Self Shadow",             "SelfShadow",            "1.0",                   1 },
        { "Clearcoat",               "Clearcoat",             "0.0",                   1 },
        { "Clearcoat Roughness",     "ClearcoatRoughness",    "0.03",                  1 },
        { "Transmission",            "Transmission",          "float3(0.0, 0.0, 0.0)", 3 },
        { "Thickness",               "Thickness",             "0.0",                   1 },
        { "Subsurface Color",        "SubsurfaceColor",       "float3(1.0, 0.3, 0.2)", 3 },
        { "Subsurface Radius",       "SubsurfaceRadius",      "0.5",                   1 },
        { "Anisotropy",              "Anisotropy",            "0.0",                   1, true },
        { "Anisotropy Rotation",     "AnisotropyRotation",    "0.0",                   1 },
        { "Second Roughness",        "SecondRoughness",       "0.5",                   1 },
        { "Second Roughness Weight", "SecondRoughnessWeight", "0.0",                   1, true },
        { "Fuzz Amount",             "FuzzAmount",            "0.0",                   1, true },
        { "Fuzz Color",              "FuzzColor",             "float3(1.0, 1.0, 1.0)", 3 },
        { "Fuzz Roughness",          "FuzzRoughness",         "0.5",                   1 },
        { "Thin Film Thickness",     "ThinFilmThickness",     "0.0",                   1, true },
        { "Thin Film IOR",           "ThinFilmIOR",           "1.5",                   1 },
        { "Bottom Normal",           "BottomNormal",          "float3(0.0, 0.0, 1.0)", 3, true,  true },
    };

    // Created by EnsureRootNodes and never offered in the palette, since a second would give the compiler two roots.
    REFLECT(NotPlaceable)
    class CMaterialOutputNode : public CMaterialGraphNode
    {
        GENERATED_BODY()
    public:
        
        CMaterialOutputNode() = default;

        FStringView GetNodeDisplayName() const override;
        FStringView GetNodeTooltip() const override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(35, 35, 200, 255); }

        void BuildNode() override;

        // Refreshes pin enable state from the material domain before pins draw, so a MaterialType change shows at once.
        void DrawNodeTitleBar() override;

        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /**
         * Every pin GenerateDefinition assigns from in the PIXEL stage, in declaration order.
         *
         * Single source of truth on purpose. The graph compiler consumes this twice -- once to collect the
         * node closure that gets emitted, once as the roots of the emit ordering -- and GenerateDefinition
         * writes an assignment for each of them unconditionally. A pin present in the assignments but
         * missing here does not degrade to "feature ignored": the assignment still references the upstream
         * node's variable, which was never declared, and the whole material fails to compile.
         *
         * WorldPositionOffsetPin is deliberately absent; it is the vertex stage's only root.
         */
        void GetPixelStagePins(TVector<CEdNodeGraphPin*>& OutPins) const;

        // Shows Pin's value as emissive on an otherwise black, unlit surface.
        static void GeneratePreviewDefinition(FMaterialCompiler& Compiler, CMaterialOutput* Pin);

        // Assigns Target.Member from the pin's connection, or from Default when it has none, and also serves Make Surface.
        static FString EmitFieldAssignment(const FString& Target, const FString& Member, CEdNodeGraphPin* Pin, const FString& Default, int32 Width);

        // Decodes a normal map sample in Target.Member into a unit tangent-space normal.
        static FString EmitNormalDecode(const FString& Target, const FString& Member);

        static bool IsPinDriven(CEdNodeGraphPin* Pin);

        bool IsDeletable() const override { return false; }

        CEdNodeGraphPin* BaseColorPin = nullptr;
        CEdNodeGraphPin* MetallicPin = nullptr;
        CEdNodeGraphPin* RoughnessPin = nullptr;
        CEdNodeGraphPin* SpecularPin = nullptr;
        CEdNodeGraphPin* EmissivePin = nullptr;
        CEdNodeGraphPin* AOPin = nullptr;
        CEdNodeGraphPin* NormalPin = nullptr;
        CEdNodeGraphPin* OpacityPin = nullptr;
        CEdNodeGraphPin* SelfShadowPin = nullptr;
        CEdNodeGraphPin* ClearcoatPin = nullptr;
        CEdNodeGraphPin* ClearcoatRoughnessPin = nullptr;
        CEdNodeGraphPin* TransmissionPin = nullptr;
        CEdNodeGraphPin* ThicknessPin = nullptr;
        CEdNodeGraphPin* SubsurfaceColorPin = nullptr;
        CEdNodeGraphPin* SubsurfaceRadiusPin = nullptr;
        CEdNodeGraphPin* AnisotropyPin = nullptr;
        CEdNodeGraphPin* AnisotropyRotationPin = nullptr;
        CEdNodeGraphPin* SecondRoughnessPin = nullptr;
        CEdNodeGraphPin* SecondRoughnessWeightPin = nullptr;
        CEdNodeGraphPin* FuzzAmountPin = nullptr;
        CEdNodeGraphPin* FuzzColorPin = nullptr;
        CEdNodeGraphPin* FuzzRoughnessPin = nullptr;
        CEdNodeGraphPin* ThinFilmThicknessPin = nullptr;
        CEdNodeGraphPin* ThinFilmIORPin = nullptr;
        CEdNodeGraphPin* BottomNormalPin = nullptr;
        CEdNodeGraphPin* WorldPositionOffsetPin = nullptr;
        // A whole surface the material starts from, which any directly driven pin then overrides.
        CEdNodeGraphPin* SurfacePin = nullptr;

    };
    
}
