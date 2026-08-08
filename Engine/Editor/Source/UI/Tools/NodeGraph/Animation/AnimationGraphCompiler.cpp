#include "AnimationGraphCompiler.h"

#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    uint16 FAnimationGraphCompiler::AddClip(CAnimation* Clip)
    {
        for (SIZE_T i = 0; i < Clips.size(); ++i)
        {
            if (Clips[i].Get() == Clip)
            {
                return (uint16)i;
            }
        }

        Clips.push_back(Clip);
        return (uint16)(Clips.size() - 1);
    }

    uint16 FAnimationGraphCompiler::AddBlendSpace(CBlendSpace* BlendSpace)
    {
        for (SIZE_T i = 0; i < BlendSpaces.size(); ++i)
        {
            if (BlendSpaces[i].Get() == BlendSpace)
            {
                return (uint16)i;
            }
        }

        BlendSpaces.push_back(BlendSpace);
        return (uint16)(BlendSpaces.size() - 1);
    }

    int32 FAnimationGraphCompiler::AddParameter(const FName& Name, EAnimGraphParamType Type, float DefaultValue)
    {
        for (SIZE_T i = 0; i < Parameters.size(); ++i)
        {
            if (Parameters[i].Name == Name)
            {
                if (Parameters[i].Type != Type)
                {
                    EdNodeGraph::FError Error;
                    Error.Name        = "Parameter Type Mismatch";
                    Error.Description = FString("Parameter '") + Name.ToString() + FString("' is referenced with conflicting types.");
                    Error.Node        = nullptr;
                    Errors.push_back(Error);
                }
                return (int32)i;
            }
        }

        FAnimGraphParameter Param;
        Param.Name         = Name;
        Param.Type         = Type;
        Param.DefaultValue = DefaultValue;
        Parameters.push_back(Param);
        return (int32)(Parameters.size() - 1);
    }

    uint16 FAnimationGraphCompiler::EmitLoadConst(float Value)
    {
        const uint16 Dst = AllocScalarReg();
        WriteOp(EAnimOp::LoadConst);
        Write(Value);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitLoadParam(uint16 ParameterIndex)
    {
        const uint16 Dst = AllocScalarReg();
        WriteOp(EAnimOp::LoadParam);
        Write(ParameterIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitScalarOp(EAnimScalarOp Op, uint16 RegA, uint16 RegB)
    {
        const uint16 Dst = AllocScalarReg();
        WriteOp(EAnimOp::ScalarOp);
        Write((uint8)Op);
        Write(RegA);
        Write(RegB);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitAdvanceClock(uint16 StateSlot, uint16 SpeedReg, uint16 ClipIndex, uint16 LoopModeReg, uint16& OutFinishedReg, uint16 SyncGroup)
    {
        const uint16 DstClock    = AllocScalarReg();
        const uint16 DstFinished = AllocScalarReg();
        WriteOp(EAnimOp::AdvanceClock);
        Write(StateSlot);
        Write(SpeedReg);
        Write(ClipIndex);
        Write(LoopModeReg);
        Write(DstClock);
        Write(DstFinished);
        Write(SyncGroup);
        OutFinishedReg = DstFinished;
        return DstClock;
    }

    uint16 FAnimationGraphCompiler::AddSyncGroup(const FName& Name)
    {
        for (SIZE_T i = 0; i < SyncGroupNames.size(); ++i)
        {
            if (SyncGroupNames[i] == Name)
            {
                return (uint16)i;
            }
        }
        SyncGroupNames.push_back(Name);
        return (uint16)(SyncGroupNames.size() - 1);
    }

    uint16 FAnimationGraphCompiler::EmitSampleAnim(uint16 ClipIndex, uint16 TimeReg)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::SampleAnim);
        Write(ClipIndex);
        Write(TimeReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitSampleBlendSpace(uint16 BlendSpaceIndex, uint16 XReg, uint16 YReg, uint16 SpeedReg, uint16 PhaseSlot)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::SampleBlendSpace);
        Write(BlendSpaceIndex);
        Write(XReg);
        Write(YReg);
        Write(SpeedReg);
        Write(PhaseSlot);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitRefPose()
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::RefPose);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitBlend(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::Blend);
        Write(PoseRegA);
        Write(PoseRegB);
        Write(AlphaReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitBlendMasked(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg, uint16 MaskIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::BlendMasked);
        Write(PoseRegA);
        Write(PoseRegB);
        Write(AlphaReg);
        Write(MaskIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitMakeAdditive(uint16 SrcPoseReg)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::MakeAdditive);
        Write(SrcPoseReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitApplyAdditive(uint16 BasePoseReg, uint16 DeltaPoseReg, uint16 AlphaReg)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::ApplyAdditive);
        Write(BasePoseReg);
        Write(DeltaPoseReg);
        Write(AlphaReg);
        Write(Dst);
        return Dst;
    }

    void FAnimationGraphCompiler::ValidateParameterKey(const FName& Name, CEdGraphNode* Node)
    {
        // No blackboard assigned, or no key referenced -> nothing to validate.
        if (Blackboard == nullptr || Name.IsNone())
        {
            return;
        }

        const FBlackboardKey* Key = Blackboard->FindKey(Name);
        if (Key == nullptr)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Unknown Blackboard Key";
            Warning.Description = FString("References blackboard key '") + Name.ToString() +
                "' which doesn't exist on the assigned blackboard (renamed or removed?). It will read the default value.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        if (!IsScalarBlackboardKey(Key->Type))
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Blackboard Key Type";
            Warning.Description = FString("Blackboard key '") + Name.ToString() +
                "' is not a scalar (Float/Int/Bool/Enum) type, so it can't drive a value parameter; it will read 0.";
            Warning.Node        = Node;
            AddWarning(Warning);
        }
    }

    int32 FAnimationGraphCompiler::ResolveBoneIndex(const FName& BoneName) const
    {
        if (Skeleton == nullptr || BoneName.IsNone())
        {
            return INDEX_NONE;
        }
        return Skeleton->FindBoneIndex(BoneName);
    }

    uint16 FAnimationGraphCompiler::EmitBoneTransform(uint16 SrcPoseReg, uint16 AlphaReg, uint16 BoneIndex,
                                                     uint16 SpaceReg, uint16 ModeReg,
                                                     const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::BoneTransform);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(BoneIndex);
        Write(SpaceReg);
        Write(ModeReg);
        Write(Translation);
        Write(Rotation);
        Write(Scale);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitTwoBoneIK(uint16 SrcPoseReg, uint16 AlphaReg,
                                                  uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                                                  uint16 RootIndex, uint16 MidIndex, uint16 EndIndex,
                                                  const FVector3& Pole)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::TwoBoneIK);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(TargetXReg);
        Write(TargetYReg);
        Write(TargetZReg);
        Write(RootIndex);
        Write(MidIndex);
        Write(EndIndex);
        Write(Pole);
        Write(Dst);
        return Dst;
    }

    void FAnimationGraphCompiler::ResolveBoneMasks(const TVector<FAnimGraphBoneMaskDef>& Defs, const FSkeletonResource* InSkeleton)
    {
        Skeleton = InSkeleton;
        BoneMasks.clear();
        BoneMaskNameToIndex.clear();

        if (Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        if (NumBones == 0)
        {
            return;
        }

        BoneMasks.reserve(Defs.size());

        for (const FAnimGraphBoneMaskDef& Def : Defs)
        {
            if (Def.Name.IsNone())
            {
                continue;
            }

            FAnimGraphBoneMask Mask;
            Mask.Weights.assign(NumBones, 0.0f);

            for (const FAnimGraphBoneMaskBone& Entry : Def.Bones)
            {
                const int32 BoneIdx = Skeleton->FindBoneIndex(Entry.BoneName);
                if (BoneIdx >= 0 && BoneIdx < NumBones)
                {
                    Mask.Weights[BoneIdx] = Math::Clamp(Entry.Weight, 0.0f, 1.0f);
                }
            }

            const int32 Index = (int32)BoneMasks.size();
            BoneMasks.push_back(Move(Mask));
            BoneMaskNameToIndex[Def.Name] = Index;
        }
    }

    int32 FAnimationGraphCompiler::FindBoneMaskIndex(const FName& Name) const
    {
        auto It = BoneMaskNameToIndex.find(Name);
        return It == BoneMaskNameToIndex.end() ? INDEX_NONE : It->second;
    }

    uint16 FAnimationGraphCompiler::AddBoneSubtreeMask(int32 RootBoneIndex, bool bInclusive)
    {
        FAnimGraphBoneMask Mask;
        const int32 NumBones = (Skeleton != nullptr) ? Skeleton->GetNumBones() : 0;
        Mask.Weights.assign(NumBones, 0.0f);

        for (int32 i = 0; i < NumBones; ++i)
        {
            bool bAffected = false;
            if (i == RootBoneIndex)
            {
                bAffected = bInclusive;
            }
            else
            {
                // Walk up the parent chain; affected if the root is an ancestor.
                for (int32 Parent = Skeleton->GetBone(i).ParentIndex; Parent >= 0; Parent = Skeleton->GetBone(Parent).ParentIndex)
                {
                    if (Parent == RootBoneIndex)
                    {
                        bAffected = true;
                        break;
                    }
                }
            }

            Mask.Weights[i] = bAffected ? 1.0f : 0.0f;
        }

        const uint16 Index = (uint16)BoneMasks.size();
        BoneMasks.push_back(Move(Mask));
        return Index;
    }

    uint16 FAnimationGraphCompiler::EmitEvalStateMachine(FAnimGraphStateMachine&& StateMachine)
    {
        const uint16 SmIndex = (uint16)StateMachines.size();
        StateMachines.push_back(Move(StateMachine));

        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::EvalStateMachine);
        Write(SmIndex);
        Write(Dst);
        return Dst;
    }

    void FAnimationGraphCompiler::EmitOutput(uint16 PoseReg)
    {
        WriteOp(EAnimOp::Output);
        Write(PoseReg);
        bEmittedOutput = true;
    }

    void FAnimationGraphCompiler::EmitHalt()
    {
        WriteOp(EAnimOp::Halt);
    }

    void FAnimationGraphCompiler::SetPinRegister(const CEdNodeGraphPin* Pin, uint16 Register)
    {
        if (Pin != nullptr)
        {
            PinRegisters[Pin] = Register;
        }
    }

    bool FAnimationGraphCompiler::TryGetPinRegister(const CEdNodeGraphPin* Pin, uint16& OutRegister) const
    {
        auto It = PinRegisters.find(Pin);
        if (It == PinRegisters.end())
        {
            return false;
        }
        OutRegister = It->second;
        return true;
    }

    void FAnimationGraphCompiler::BuildGraph(CAnimationGraph* OutGraph)
    {
        if (OutGraph == nullptr)
        {
            return;
        }

        if (!bEmittedOutput)
        {
            EmitHalt();
        }

        OutGraph->Bytecode           = Bytecode;
        OutGraph->Clips              = Clips;
        OutGraph->BlendSpaces        = BlendSpaces;
        OutGraph->Parameters         = Parameters;
        OutGraph->BoneMasks          = BoneMasks;
        OutGraph->StateMachines      = StateMachines;
        OutGraph->NumScalarRegisters = NextScalarReg;
        OutGraph->NumPoseRegisters   = NextPoseReg;
        OutGraph->NumStateSlots      = NextStateSlot;
        OutGraph->NumSyncGroups      = (uint16)SyncGroupNames.size();
        OutGraph->BytecodeVersion    = kAnimBytecodeVersion;

        OutGraph->ResolveTransitionParameters();
    }
}
