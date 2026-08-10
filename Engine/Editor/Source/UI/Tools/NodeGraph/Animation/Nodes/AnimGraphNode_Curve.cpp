#include "AnimGraphNode_Curve.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_GetCurve::BuildNode()
    {
        PosePin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        ValuePin = CreateAnimPin("Value", ENodePinDirection::Output, EAnimPinType::Value);
    }

    void CAnimGraphNode_GetCurve::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 PoseReg   = ResolvePoseInput(PosePin, Compiler);
        const int32  CurveSlot = Compiler.AddCurve(CurveName);

        if (CurveSlot == INDEX_NONE)
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Missing Curve Name";
            Error.Description = "Get Curve node has no curve name assigned; it will read 0.";
            Error.Node        = this;
            Compiler.AddError(Error);

            Compiler.SetPinRegister(ValuePin, Compiler.EmitLoadConst(0.0f));
            return;
        }

        Compiler.SetPinRegister(ValuePin, Compiler.EmitGetCurve(PoseReg, (uint16)CurveSlot));
    }

    void CAnimGraphNode_SetCurve::BuildNode()
    {
        PosePin   = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        ValuePin  = CreateAnimPin("Value", ENodePinDirection::Input, EAnimPinType::Value);
        ResultPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(ValuePin);
    }

    void CAnimGraphNode_SetCurve::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 PoseReg   = ResolvePoseInput(PosePin, Compiler);
        const uint16 ValueReg  = ResolveValueInput(ValuePin, Compiler);
        const int32  CurveSlot = Compiler.AddCurve(CurveName);

        if (CurveSlot == INDEX_NONE)
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Missing Curve Name";
            Error.Description = "Set Curve node has no curve name assigned; the pose passes through unchanged.";
            Error.Node        = this;
            Compiler.AddError(Error);

            Compiler.SetPinRegister(ResultPin, PoseReg);
            return;
        }

        Compiler.SetPinRegister(ResultPin, Compiler.EmitSetCurve(PoseReg, (uint16)CurveSlot, ValueReg));
    }
}
