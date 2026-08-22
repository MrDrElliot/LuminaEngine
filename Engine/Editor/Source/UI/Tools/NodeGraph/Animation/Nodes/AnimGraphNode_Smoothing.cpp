#include "AnimGraphNode_Smoothing.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_Inertialization::BuildNode()
    {
        PoseInputPin = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        RequestPin   = CreateAnimPin("Request", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        DurationPin  = CreateAnimPin("Duration", ENodePinDirection::Input, EAnimPinType::Value, 0.2f);
        ResultPin    = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(RequestPin);
        BindFloatPinEditor(DurationPin);
    }

    void CAnimGraphNode_Inertialization::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Src      = ResolvePoseInput(PoseInputPin, Compiler);
        const uint16 Request  = ResolveValueInput(RequestPin, Compiler);
        const uint16 Duration = ResolveValueInput(DurationPin, Compiler);

        const uint16 Result = Compiler.EmitInertialize(Src, Request, Duration, Compiler.AllocInertializerNode());
        Compiler.SetPinRegister(ResultPin, Result);
    }

    void CAnimGraphNode_DeadBlending::BuildNode()
    {
        PoseInputPin = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        RequestPin   = CreateAnimPin("Request", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        DurationPin  = CreateAnimPin("Duration", ENodePinDirection::Input, EAnimPinType::Value, 0.2f);
        HalfLifePin  = CreateAnimPin("Half Life", ENodePinDirection::Input, EAnimPinType::Value, 0.1f);
        ResultPin    = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(RequestPin);
        BindFloatPinEditor(DurationPin);
        BindFloatPinEditor(HalfLifePin);
    }

    void CAnimGraphNode_DeadBlending::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Src      = ResolvePoseInput(PoseInputPin, Compiler);
        const uint16 Request  = ResolveValueInput(RequestPin, Compiler);
        const uint16 Duration = ResolveValueInput(DurationPin, Compiler);
        const uint16 HalfLife = ResolveValueInput(HalfLifePin, Compiler);

        const uint16 Result = Compiler.EmitDeadBlend(Src, Request, Duration, HalfLife, Compiler.AllocDeadBlendNode());
        Compiler.SetPinRegister(ResultPin, Result);
    }
}
