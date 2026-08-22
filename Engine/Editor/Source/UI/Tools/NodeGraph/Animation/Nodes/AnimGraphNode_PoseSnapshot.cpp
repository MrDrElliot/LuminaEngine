#include "AnimGraphNode_PoseSnapshot.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_SavePoseSnapshot::BuildNode()
    {
        PoseInputPin = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        RequestPin   = CreateAnimPin("Request", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        ResultPin    = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(RequestPin);
    }

    void CAnimGraphNode_SavePoseSnapshot::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Src     = ResolvePoseInput(PoseInputPin, Compiler);
        const uint16 Request = ResolveValueInput(RequestPin, Compiler);
        const uint16 Index   = Compiler.AddPoseSnapshot(SnapshotName);

        Compiler.SetPinRegister(ResultPin, Compiler.EmitSavePoseSnapshot(Src, Request, Index));
    }

    FString CAnimGraphNode_SavePoseSnapshot::GetNodeTitleText() const
    {
        return SnapshotName.IsNone() ? FString("Save Pose Snapshot") : Format("Save {}", SnapshotName.c_str());
    }

    void CAnimGraphNode_PoseSnapshot::BuildNode()
    {
        PosePin = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    void CAnimGraphNode_PoseSnapshot::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        Compiler.SetPinRegister(PosePin, Compiler.EmitLoadPoseSnapshot(Compiler.AddPoseSnapshot(SnapshotName)));
    }

    FString CAnimGraphNode_PoseSnapshot::GetNodeTitleText() const
    {
        return SnapshotName.IsNone() ? FString("Pose Snapshot") : FString(SnapshotName.c_str());
    }
}
