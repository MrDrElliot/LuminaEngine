#include "MaterialNode_Wind.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_WindAnimation::BuildNode()
    {
        Position   = MakeIn(this, "Position",   EMaterialInputType::Float3);
        Direction  = MakeIn(this, "Direction",  EMaterialInputType::Float3);
        Strength   = MakeIn(this, "Strength");
        Speed      = MakeIn(this, "Speed");
        Frequency  = MakeIn(this, "Frequency");
        Lacunarity = MakeIn(this, "Lacunarity");
        Gain       = MakeIn(this, "Gain");
        Mask       = MakeIn(this, "Mask");
        Phase      = MakeIn(this, "Phase");
        Gustiness  = MakeIn(this, "Gustiness");
        FadeStart  = MakeIn(this, "Fade Start");
        FadeEnd    = MakeIn(this, "Fade End");

        OffsetOut = MakeOut(this, "Offset", EMaterialInputType::Float3);
        WeightOut = MakeOut(this, "Weight", EMaterialInputType::Float);
        NoiseOut  = MakeOut(this, "Noise",  EMaterialInputType::Float);
    }

    void CMaterialExpression_WindAnimation::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        FMaterialCompiler::FWindInputs Inputs;
        Inputs.Position   = Position;
        Inputs.Direction  = Direction;
        Inputs.Strength   = Strength;
        Inputs.Speed      = Speed;
        Inputs.Frequency  = Frequency;
        Inputs.Lacunarity = Lacunarity;
        Inputs.Gain       = Gain;
        Inputs.Mask       = Mask;
        Inputs.Phase      = Phase;
        Inputs.Gustiness  = Gustiness;
        Inputs.FadeStart  = FadeStart;
        Inputs.FadeEnd    = FadeEnd;

        Compiler.WindAnimation(this, Inputs, Math::Clamp(Octaves, 1, 6), bLODGate,
                               OffsetOut, WeightOut, NoiseOut);
    }
}
