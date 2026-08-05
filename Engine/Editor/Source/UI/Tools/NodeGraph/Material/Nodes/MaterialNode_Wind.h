#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Wind.generated.h"

namespace Lumina
{
    /** Multi-frequency procedural wind displacement for the vertex stage. */
    REFLECT()
    class CMaterialExpression_WindAnimation : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vertex"; }
        FStringView GetNodeDisplayName() const override { return "WindAnimation"; }
        FStringView GetNodeTooltip() const override
        {
            return "World-space wind displacement for foliage, cloth and grass. Feed Offset into World "
                   "Position Offset. The field is a sum of Octaves sine bands (whole-plant sway, branch "
                   "motion, leaf flutter) plus a slower gust envelope rolling along the wind, so the "
                   "motion never reads as a single pendulum.\n\n"
                   "Mask is the per-vertex stiffness weight -- vertex color or a UV gradient, 0 at the "
                   "roots and 1 at the tips. Without it the mesh translates rigidly instead of bending. "
                   "Drive Phase from object position or entity ID so instances do not sway in lockstep.\n\n"
                   "The LOD gate fades the displacement out between Fade Start and Fade End (world units "
                   "from the camera) and skips the whole evaluation past the end, which is where the "
                   "vertex cost actually goes away. Distance is measured from the MAIN camera in every "
                   "pass, so shadow and depth geometry fade in step with the surface.\n\n"
                   "Vertex stage only: it is reachable from World Position Offset and nothing else.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Number of frequency bands summed into the wind field. Each octave is finer, weaker and
         *  quicker than the last. Baked into the shader rather than exposed as a pin so the loop
         *  unrolls; a dynamic count would leave a real loop in every vertex of every pass. */
        PROPERTY(Editable, Category = "Quality", ClampMin = "1", ClampMax = "6")
        int32 Octaves = 3;

        /** Fade the displacement out with distance from the camera and skip it entirely past Fade End.
         *  Turn off only for something that must animate at any range; the gate is most of what keeps
         *  wind affordable on dense foliage. */
        PROPERTY(Editable, Category = "LOD")
        bool bLODGate = true;

        CMaterialInput* Position   = nullptr;
        CMaterialInput* Direction  = nullptr;
        CMaterialInput* Strength   = nullptr;
        CMaterialInput* Speed      = nullptr;
        CMaterialInput* Frequency  = nullptr;
        CMaterialInput* Lacunarity = nullptr;
        CMaterialInput* Gain       = nullptr;
        CMaterialInput* Mask       = nullptr;
        CMaterialInput* Phase      = nullptr;
        CMaterialInput* Gustiness  = nullptr;
        CMaterialInput* FadeStart  = nullptr;
        CMaterialInput* FadeEnd    = nullptr;

        CMaterialOutput* OffsetOut = nullptr;
        CMaterialOutput* WeightOut = nullptr;
        CMaterialOutput* NoiseOut  = nullptr;
    };
}
