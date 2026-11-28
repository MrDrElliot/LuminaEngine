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
                   "Use Global Wind takes the direction and strength from the Weather component's wind instead "
                   "of the Direction pin, scaling Strength by how hard it blows against Reference Wind Speed. "
                   "Its gusts sweep across the world and blockers calm it, so the node's own Gustiness is not "
                   "used then. Off, the node behaves as before and needs no weather.\n\n"
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

        // Off by default, so existing materials and worlds without weather keep their authored sway.
        PROPERTY(Editable, Category = "Wind", ToolTip = "Follow the Weather component's wind for direction, strength, gusts and shelter instead of the Direction and Gustiness pins.")
        bool bUseGlobalWind = false;

        PROPERTY(Editable, Category = "Wind", EditCondition = "bUseGlobalWind", ClampMin = "0.1", ClampMax = "60", ToolTip = "Wind speed in meters per second at which the mesh bends by exactly Strength, twice as far at double the speed.")
        float ReferenceWindSpeed = 5.0f;

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

    // The weather's wind at a point, for steering foliage sway, flags, particles or panning textures.
    REFLECT()
    class CMaterialExpression_GlobalWind : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Scene"; }
        FStringView GetNodeDisplayName() const override { return "GlobalWind"; }
        FStringView GetNodeTooltip() const override
        {
            return "The wind the Weather component plays, sampled at Position (world position when unconnected). "
                   "Direction is the unit XZ direction the air moves toward and Speed is in meters per second, both "
                   "including the gust passing that point, since gusts sweep across the world along the wind. Gust "
                   "is how far that gust pushes above or below the steady wind, and Shelter is 1 inside a Wind "
                   "Blocker that stops all wind. Without a Weather component the wind is still.\n\n"
                   "Feed Direction and Speed into WindAnimation, or scale World Position Offset by Speed.";
        }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Position = nullptr;

        CMaterialOutput* DirectionOut = nullptr;
        CMaterialOutput* SpeedOut     = nullptr;
        CMaterialOutput* GustOut      = nullptr;
        CMaterialOutput* ShelterOut   = nullptr;
        CMaterialOutput* VelocityOut  = nullptr;
    };
}
