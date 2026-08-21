#include "AnimGraphNode_Additive.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_MakeAdditive::BuildNode()
    {
        PoseInputPin   = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        BasePosePin    = CreateAnimPin("Base", ENodePinDirection::Input, EAnimPinType::Pose);
        DeltaOutputPin = CreateAnimPin("Delta", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    void CAnimGraphNode_MakeAdditive::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg = ResolvePoseInput(PoseInputPin, Compiler);

        // An unconnected Base means the bind pose, which the VM reads straight off the skeleton.
        const uint16 BaseReg = (BasePosePin != nullptr && BasePosePin->HasConnection())
            ? ResolvePoseInput(BasePosePin, Compiler)
            : kAnimNoPoseRegister;

        const uint16 DstReg = Compiler.EmitMakeAdditive(SrcReg, BaseReg, Space);
        Compiler.SetPinRegister(DeltaOutputPin, DstReg);
    }

    void CAnimGraphNode_ApplyAdditive::BuildNode()
    {
        BasePin   = CreateAnimPin("Base", ENodePinDirection::Input, EAnimPinType::Pose);
        DeltaPin  = CreateAnimPin("Delta", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin  = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        ResultPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
    }

    void CAnimGraphNode_ApplyAdditive::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Base   = ResolvePoseInput(BasePin, Compiler);
        const uint16 Delta  = ResolvePoseInput(DeltaPin, Compiler);
        const uint16 Alpha  = ResolveValueInput(AlphaPin, Compiler);
        const uint16 Result = Compiler.EmitApplyAdditive(Base, Delta, Alpha);
        Compiler.SetPinRegister(ResultPin, Result);
    }
}
