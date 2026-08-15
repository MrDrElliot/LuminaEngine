#include "AnimGraphNode_Slot.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_Slot::BuildNode()
    {
        SourcePin = CreateAnimPin("Source", ENodePinDirection::Input, EAnimPinType::Pose);
        PosePin   = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    void CAnimGraphNode_Slot::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        if (SlotName.IsNone())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Slot Name";
            NodeError.Description = "Slot node has no slot name; no montage can ever play through it.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        const uint16 SrcPoseReg = ResolvePoseInput(SourcePin, Compiler);
        const uint16 SlotIndex  = Compiler.AddSlot(SlotName);
        const uint16 PoseReg    = Compiler.EmitEvalSlot(SrcPoseReg, SlotIndex);

        Compiler.SetPinRegister(PosePin, PoseReg);
    }
}
