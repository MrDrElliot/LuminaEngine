#include "AnimGraphNode_Blend.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_Blend::BuildNode()
    {
        PoseAPin  = CreateAnimPin("Pose A", ENodePinDirection::Input, EAnimPinType::Pose);
        PoseBPin  = CreateAnimPin("Pose B", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin  = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        ResultPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
    }

    void CAnimGraphNode_Blend::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 PoseA   = ResolvePoseInput(PoseAPin, Compiler);
        const uint16 PoseB   = ResolvePoseInput(PoseBPin, Compiler);
        const uint16 Alpha   = ResolveAlphaInput(AlphaPin, Compiler, AlphaEasing);
        const uint16 Result  = Compiler.EmitBlend(PoseA, PoseB, Alpha);

        Compiler.SetPinRegister(ResultPin, Result);
    }
}
