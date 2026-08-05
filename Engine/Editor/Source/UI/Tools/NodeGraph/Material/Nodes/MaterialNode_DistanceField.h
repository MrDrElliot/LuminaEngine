#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_DistanceField.generated.h"

namespace Lumina
{
    /** Samples the primitive's own baked signed distance field at a world position. */
    REFLECT()
    class CMaterialExpression_MeshDistanceField : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Distance Field"; }
        FStringView GetNodeDisplayName() const override { return "MeshDistanceField"; }
        FStringView GetNodeTooltip() const override
        {
            return "Signed distance from a world position to this mesh's surface, in world units: negative "
                   "inside, positive outside. Gradient is the outward normal of the field. Valid is 0 when "
                   "the mesh has no distance field or the point falls outside the baked volume -- always "
                   "branch on it, because Distance reads as a large positive constant in that case.\n\n"
                   "Enable the field on the mesh asset (Distance Field > Enabled), or at import.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Position = nullptr;

        CMaterialOutput* DistanceOut = nullptr;
        CMaterialOutput* GradientOut = nullptr;
        CMaterialOutput* ValidOut    = nullptr;
    };

    /** Cone-traced self-occlusion against the primitive's own field. */
    REFLECT()
    class CMaterialExpression_MeshDistanceFieldOcclusion : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Distance Field"; }
        FStringView GetNodeDisplayName() const override { return "MeshDistanceFieldAO"; }
        FStringView GetNodeTooltip() const override
        {
            return "Ambient occlusion from the mesh's own distance field: a cone traced along the normal, "
                   "darkening creases and areas under the mesh's own overhangs. Follows the geometry with no "
                   "baked AO map and no UV space.\n\n"
                   "Radius and the trace are in fractions of the field's volume, so one value works across "
                   "differently sized meshes. Cost is Steps texture fetches per pixel.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** March steps. The cone is sphere-traced, so this is an upper bound rather than a fixed cost:
         *  a ray that leaves the volume or converges stops early. Baked into the shader, not a parameter,
         *  because a dynamic loop count would stop the compiler unrolling the march. */
        PROPERTY(Editable, Category = "Quality", ClampMin = "2", ClampMax = "64")
        int32 Steps = 12;

        CMaterialInput* Normal    = nullptr;
        CMaterialInput* Radius    = nullptr;
        CMaterialInput* ConeAngle = nullptr;
        CMaterialInput* Intensity = nullptr;

        CMaterialOutput* OcclusionOut = nullptr;
    };

    /** Marches into the mesh along -Normal to measure how thick it is under this pixel. */
    REFLECT()
    class CMaterialExpression_MeshDistanceFieldThickness : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Distance Field"; }
        FStringView GetNodeDisplayName() const override { return "MeshDistanceFieldThickness"; }
        FStringView GetNodeTooltip() const override
        {
            return "How much mesh sits behind this pixel, measured by marching into the surface along the "
                   "inverted normal. Drives subsurface scattering and translucency: an ear or a leaf reads "
                   "thin and transmits, a torso reads thick and does not.\n\n"
                   "Thickness is in world units; Normalized is the same value over the march distance, ready "
                   "to use as a mask. Returns 0 for a two-sided field, which has no inside.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Quality", ClampMin = "2", ClampMax = "64")
        int32 Steps = 12;

        CMaterialInput* Normal      = nullptr;
        CMaterialInput* MaxDistance = nullptr;

        CMaterialOutput* ThicknessOut  = nullptr;
        CMaterialOutput* NormalizedOut = nullptr;
    };
}
