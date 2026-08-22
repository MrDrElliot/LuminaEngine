#include "AnimGraphNode_IK.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        // Every solver here fails the same way, so the report reads the same wherever it comes from.
        void ReportUnknownBone(FAnimationGraphCompiler& Compiler, CEdGraphNode* Node, const char* NodeName, const FName& Bone)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Unknown IK Bone";
            NodeError.Description = FString(NodeName) + " references '" + Bone.ToString() +
                "', which is not a bone on the graph's skeleton.";
            NodeError.Node        = Node;
            Compiler.AddError(NodeError);
        }
    }

    void CAnimGraphNode_FABRIK::BuildNode()
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

    void CAnimGraphNode_FABRIK::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);

        const int32 RootIdx = Compiler.ResolveBoneIndex(RootBone);
        const int32 TipIdx  = Compiler.ResolveBoneIndex(TipBone);

        if (RootIdx == INDEX_NONE || TipIdx == INDEX_NONE)
        {
            ReportUnknownBone(Compiler, this, "FABRIK", RootIdx == INDEX_NONE ? RootBone : TipBone);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        // The chain is walked tip to root at runtime, so an unrelated pair simply solves nothing.
        const FSkeletonResource* Skeleton = Compiler.GetSkeleton();
        if (Skeleton != nullptr)
        {
            bool bDescends = false;
            for (int32 Cursor = TipIdx; Cursor >= 0; Cursor = Skeleton->GetBone(Cursor).ParentIndex)
            {
                if (Cursor == RootIdx)
                {
                    bDescends = true;
                    break;
                }
            }

            if (!bDescends)
            {
                EdNodeGraph::FError NodeError;
                NodeError.Name        = "Bad FABRIK Chain";
                NodeError.Description = FString("'") + TipBone.ToString() + "' does not descend from '" +
                    RootBone.ToString() + "', so there is no chain between them.";
                NodeError.Node        = this;
                Compiler.AddError(NodeError);
                Compiler.SetPinRegister(PoseOutPin, SrcReg);
                return;
            }
        }

        const uint16 AlphaReg   = ResolveValueInput(AlphaPin, Compiler);
        const uint16 TargetXReg = ResolveValueInput(TargetXPin, Compiler);
        const uint16 TargetYReg = ResolveValueInput(TargetYPin, Compiler);
        const uint16 TargetZReg = ResolveValueInput(TargetZPin, Compiler);

        const uint16 ResultReg = Compiler.EmitFABRIK(SrcReg, AlphaReg, TargetXReg, TargetYReg, TargetZReg,
                                                     (uint16)RootIdx, (uint16)TipIdx, (uint16)Math::Clamp(Iterations, 1, 32));
        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }

    void CAnimGraphNode_LookAt::BuildNode()
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

    void CAnimGraphNode_LookAt::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg  = ResolvePoseInput(PoseInPin, Compiler);
        const int32  BoneIdx = Compiler.ResolveBoneIndex(Bone);

        if (BoneIdx == INDEX_NONE)
        {
            ReportUnknownBone(Compiler, this, "Look At", Bone);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const uint16 AlphaReg   = ResolveValueInput(AlphaPin, Compiler);
        const uint16 TargetXReg = ResolveValueInput(TargetXPin, Compiler);
        const uint16 TargetYReg = ResolveValueInput(TargetYPin, Compiler);
        const uint16 TargetZReg = ResolveValueInput(TargetZPin, Compiler);

        const float MaxRadians = Math::Radians(Math::Clamp(MaxAngle, 0.0f, 180.0f));

        const uint16 ResultReg = Compiler.EmitLookAt(SrcReg, AlphaReg, TargetXReg, TargetYReg, TargetZReg,
                                                     (uint16)BoneIdx, LocalForward, MaxRadians);
        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }

    void CAnimGraphNode_FootIK::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        OffsetXPin = CreateAnimPin("Offset X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        OffsetYPin = CreateAnimPin("Offset Y", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        OffsetZPin = CreateAnimPin("Offset Z", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        NormalXPin = CreateAnimPin("Normal X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        NormalYPin = CreateAnimPin("Normal Y", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        NormalZPin = CreateAnimPin("Normal Z", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        AlignPin   = CreateAnimPin("Align To Normal", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
        BindFloatPinEditor(OffsetXPin);
        BindFloatPinEditor(OffsetYPin);
        BindFloatPinEditor(OffsetZPin);
        BindFloatPinEditor(NormalXPin);
        BindFloatPinEditor(NormalYPin);
        BindFloatPinEditor(NormalZPin);
        BindFloatPinEditor(AlignPin);
    }

    void CAnimGraphNode_FootIK::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);

        const int32 ThighIdx = Compiler.ResolveBoneIndex(ThighBone);
        const int32 CalfIdx  = Compiler.ResolveBoneIndex(CalfBone);
        const int32 FootIdx  = Compiler.ResolveBoneIndex(FootBone);

        if (ThighIdx == INDEX_NONE || CalfIdx == INDEX_NONE || FootIdx == INDEX_NONE)
        {
            const FName& Missing = ThighIdx == INDEX_NONE ? ThighBone : (CalfIdx == INDEX_NONE ? CalfBone : FootBone);
            ReportUnknownBone(Compiler, this, "Foot IK", Missing);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const FSkeletonResource* Skeleton = Compiler.GetSkeleton();
        if (Skeleton != nullptr &&
            (Skeleton->GetBone(CalfIdx).ParentIndex != ThighIdx || Skeleton->GetBone(FootIdx).ParentIndex != CalfIdx))
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Bad Leg Chain";
            NodeError.Description = FString("Foot IK needs the leg parented as thigh to calf to foot on the skeleton.");
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const uint16 AlphaReg   = ResolveValueInput(AlphaPin, Compiler);
        const uint16 OffsetXReg = ResolveValueInput(OffsetXPin, Compiler);
        const uint16 OffsetYReg = ResolveValueInput(OffsetYPin, Compiler);
        const uint16 OffsetZReg = ResolveValueInput(OffsetZPin, Compiler);
        const uint16 NormalXReg = ResolveValueInput(NormalXPin, Compiler);
        const uint16 NormalYReg = ResolveValueInput(NormalYPin, Compiler);
        const uint16 NormalZReg = ResolveValueInput(NormalZPin, Compiler);
        const uint16 AlignReg   = ResolveValueInput(AlignPin, Compiler);

        const uint16 ResultReg = Compiler.EmitFootIK(SrcReg, AlphaReg,
                                                     OffsetXReg, OffsetYReg, OffsetZReg,
                                                     NormalXReg, NormalYReg, NormalZReg,
                                                     AlignReg, (uint16)ThighIdx, (uint16)CalfIdx, (uint16)FootIdx,
                                                     FootUpAxis);
        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }

    void CAnimGraphNode_TranslateBone::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        OffsetXPin = CreateAnimPin("Offset X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        OffsetYPin = CreateAnimPin("Offset Y", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        OffsetZPin = CreateAnimPin("Offset Z", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
        BindFloatPinEditor(OffsetXPin);
        BindFloatPinEditor(OffsetYPin);
        BindFloatPinEditor(OffsetZPin);
    }

    void CAnimGraphNode_TranslateBone::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg  = ResolvePoseInput(PoseInPin, Compiler);
        const int32  BoneIdx = Compiler.ResolveBoneIndex(Bone);

        if (BoneIdx == INDEX_NONE)
        {
            ReportUnknownBone(Compiler, this, "Translate Bone", Bone);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const uint16 AlphaReg   = ResolveValueInput(AlphaPin, Compiler);
        const uint16 OffsetXReg = ResolveValueInput(OffsetXPin, Compiler);
        const uint16 OffsetYReg = ResolveValueInput(OffsetYPin, Compiler);
        const uint16 OffsetZReg = ResolveValueInput(OffsetZPin, Compiler);

        const uint16 ResultReg = Compiler.EmitTranslateBone(SrcReg, AlphaReg, OffsetXReg, OffsetYReg, OffsetZReg, (uint16)BoneIdx);
        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }
}
