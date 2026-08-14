#include "AnimGraphNode_TwoBoneIK.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    void CAnimGraphNode_TwoBoneIK::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        TargetXPin = CreateAnimPin("Target X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        TargetYPin = CreateAnimPin("Target Y", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        TargetZPin = CreateAnimPin("Target Z", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
        BindFloatPinEditor(TargetXPin);
        BindFloatPinEditor(TargetYPin);
        BindFloatPinEditor(TargetZPin);
    }

    void CAnimGraphNode_TwoBoneIK::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const int32 RootIdx = Compiler.ResolveBoneIndex(RootBone);
        const int32 MidIdx  = Compiler.ResolveBoneIndex(MidBone);
        const int32 EndIdx  = Compiler.ResolveBoneIndex(EndBone);

        const FSkeletonResource* Skeleton = Compiler.GetSkeleton();

        bool bChainOk = (RootIdx != INDEX_NONE && MidIdx != INDEX_NONE && EndIdx != INDEX_NONE);
        if (bChainOk && Skeleton != nullptr)
        {
            if (Skeleton->GetBone(MidIdx).ParentIndex != RootIdx ||
                Skeleton->GetBone(EndIdx).ParentIndex != MidIdx)
            {
                EdNodeGraph::FError NodeError;
                NodeError.Name        = "Bad IK Chain";
                NodeError.Description = FString("Two-Bone IK chain is not parented as Root -> Mid -> End on the skeleton.");
                NodeError.Node        = this;
                Compiler.AddError(NodeError);
                bChainOk = false;
            }
        }
        else
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Unknown IK Bone";
            NodeError.Description = FString("Two-Bone IK references a bone that doesn't exist on the graph's skeleton.");
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);

        if (!bChainOk)
        {
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const uint16 AlphaReg   = ResolveValueInput(AlphaPin, Compiler);
        const uint16 TargetXReg = ResolveValueInput(TargetXPin, Compiler);
        const uint16 TargetYReg = ResolveValueInput(TargetYPin, Compiler);
        const uint16 TargetZReg = ResolveValueInput(TargetZPin, Compiler);

        const uint16 ResultReg = Compiler.EmitTwoBoneIK(SrcReg, AlphaReg,
                                                        TargetXReg, TargetYReg, TargetZReg,
                                                        (uint16)RootIdx, (uint16)MidIdx, (uint16)EndIdx,
                                                        PoleVector);

        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }
}
