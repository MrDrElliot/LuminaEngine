#include "AnimGraphNode_Remap.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_Remap::BuildNode()
    {
        ValuePin  = CreateAnimPin("Value", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        InMinPin  = CreateAnimPin("In Min", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        InMaxPin  = CreateAnimPin("In Max", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        OutMinPin = CreateAnimPin("Out Min", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        OutMaxPin = CreateAnimPin("Out Max", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        ResultPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Value);

        BindFloatPinEditor(ValuePin);
        BindFloatPinEditor(InMinPin);
        BindFloatPinEditor(InMaxPin);
        BindFloatPinEditor(OutMinPin);
        BindFloatPinEditor(OutMaxPin);
    }

    void CAnimGraphNode_Remap::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 ValueReg  = ResolveValueInput(ValuePin, Compiler);
        const uint16 InMinReg  = ResolveValueInput(InMinPin, Compiler);
        const uint16 InMaxReg  = ResolveValueInput(InMaxPin, Compiler);
        const uint16 OutMinReg = ResolveValueInput(OutMinPin, Compiler);
        const uint16 OutMaxReg = ResolveValueInput(OutMaxPin, Compiler);

        // A divide by zero on a degenerate input range resolves to 0 inside the VM.
        const uint16 Numerator   = Compiler.EmitScalarOp(EAnimScalarOp::Sub, ValueReg, InMinReg);
        const uint16 Denominator = Compiler.EmitScalarOp(EAnimScalarOp::Sub, InMaxReg, InMinReg);
        uint16 NormalizedReg     = Compiler.EmitScalarOp(EAnimScalarOp::Div, Numerator, Denominator);

        if (bClampToRange)
        {
            NormalizedReg = Compiler.EmitScalarOp(EAnimScalarOp::Clamp01, NormalizedReg, NormalizedReg);
        }

        // Scales into the output range as OutMin plus Normalized times the span.
        const uint16 OutRangeReg = Compiler.EmitScalarOp(EAnimScalarOp::Sub, OutMaxReg, OutMinReg);
        const uint16 ScaledReg   = Compiler.EmitScalarOp(EAnimScalarOp::Mul, NormalizedReg, OutRangeReg);
        const uint16 ResultReg   = Compiler.EmitScalarOp(EAnimScalarOp::Add, ScaledReg, OutMinReg);

        Compiler.SetPinRegister(ResultPin, ResultReg);
    }
}
