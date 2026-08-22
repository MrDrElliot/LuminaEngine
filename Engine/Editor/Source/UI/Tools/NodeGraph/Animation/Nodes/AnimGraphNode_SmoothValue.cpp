#include "AnimGraphNode_SmoothValue.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_SmoothValue::BuildNode()
    {
        ValuePin    = CreateAnimPin("Value", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        HalfLifePin = CreateAnimPin("Half Life", ENodePinDirection::Input, EAnimPinType::Value, 0.1f);
        ResultPin   = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Value);

        BindFloatPinEditor(ValuePin);
        BindFloatPinEditor(HalfLifePin);
    }

    void CAnimGraphNode_SmoothValue::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Value    = ResolveValueInput(ValuePin, Compiler);
        const uint16 HalfLife = ResolveValueInput(HalfLifePin, Compiler);

        Compiler.SetPinRegister(ResultPin, Compiler.EmitSmoothScalar(Value, HalfLife));
    }
}
