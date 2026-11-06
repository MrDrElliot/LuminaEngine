#include "AnimationGraphCompiler.h"

#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    uint16 FAnimationGraphCompiler::AddClip(CAnimation* Clip)
    {
        for (int32 i = 0; i < Clips.Num(); ++i)
        {
            if (Clips[i].Get() == Clip)
            {
                return (uint16)i;
            }
        }

        Clips.Add(Clip);
        ClipCurveMaps.Add(BuildClipCurveMap(Clip));
        return (uint16)(Clips.Num() - 1);
    }

    uint16 FAnimationGraphCompiler::AddBlendSpace(CBlendSpace* BlendSpace)
    {
        for (int32 i = 0; i < BlendSpaces.Num(); ++i)
        {
            if (BlendSpaces[i].Get() == BlendSpace)
            {
                return (uint16)i;
            }
        }

        FAnimGraphBlendSpaceCurveMap CurveMap;
        if (BlendSpace != nullptr)
        {
            CurveMap.SampleMaps.Reserve(BlendSpace->Samples.Num());
            for (const SBlendSpaceSample& Sample : BlendSpace->Samples)
            {
                CurveMap.SampleMaps.Add(BuildClipCurveMap(Sample.Animation.Get()));
            }
        }

        BlendSpaces.Add(BlendSpace);
        BlendSpaceCurveMaps.Add(Move(CurveMap));
        return (uint16)(BlendSpaces.Num() - 1);
    }

    int32 FAnimationGraphCompiler::AddCurve(const FName& Name)
    {
        if (Name.IsNone())
        {
            return Constants::kIndexNone;
        }

        auto It = CurveNameToIndex.Find(Name);
        if (It != nullptr)
        {
            return (*It);
        }

        const int32 Index = CurveNames.Num();
        CurveNames.Add(Name);
        CurveNameToIndex[Name] = Index;
        return Index;
    }

    FAnimGraphClipCurveMap FAnimationGraphCompiler::BuildClipCurveMap(CAnimation* Clip)
    {
        FAnimGraphClipCurveMap Map;
        if (Clip == nullptr)
        {
            return Map;
        }

        const TVector<FAnimationCurve>& Curves = Clip->GetCurves();
        Map.Slots.Reserve(Curves.Num());
        for (const FAnimationCurve& Curve : Curves)
        {
            Map.Slots.Add(AddCurve(Curve.Name));
        }
        return Map;
    }

    int32 FAnimationGraphCompiler::AddParameter(const FName& Name, EAnimGraphParamType Type, float DefaultValue)
    {
        for (int32 i = 0; i < Parameters.Num(); ++i)
        {
            if (Parameters[i].Name == Name)
            {
                if (Parameters[i].Type != Type)
                {
                    EdNodeGraph::FError Error;
                    Error.Name        = "Parameter Type Mismatch";
                    Error.Description = FString("Parameter '") + Name.ToString() + FString("' is referenced with conflicting types.");
                    Error.Node        = nullptr;
                    Errors.Add(Error);
                }
                return (int32)i;
            }
        }

        FAnimGraphParameter Param;
        Param.Name         = Name;
        Param.Type         = Type;
        Param.DefaultValue = DefaultValue;
        Parameters.Add(Param);
        return Parameters.Num() - 1;
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

    int32 FAnimationGraphCompiler::AddObjectParameter(const FName& Name, EAnimObjectParamType Type)
    {
        for (int32 i = 0; i < ObjectParameters.Num(); ++i)
        {
            if (ObjectParameters[i].Name == Name)
            {
                return (int32)i;
            }
        }

        SAnimGraphObjectParameter Param;
        Param.Name = Name;
        Param.Type = Type;
        ObjectParameters.Add(Param);
        return ObjectParameters.Num() - 1;
    }

    uint16 FAnimationGraphCompiler::AddObjectConstant(CObject* Value)
    {
        for (int32 i = 0; i < ObjectConstants.Num(); ++i)
        {
            if (ObjectConstants[i].Get() == Value)
            {
                return (uint16)i;
            }
        }
        ObjectConstants.Add(Value);
        return (uint16)(ObjectConstants.Num() - 1);
    }

    uint16 FAnimationGraphCompiler::EmitLoadObjectParam(uint16 ObjectParameterIndex)
    {
        const uint16 Dst = AllocObjectReg();
        WriteOp(EAnimOp::LoadObjectParam);
        Write(ObjectParameterIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitLoadObjectConst(uint16 ObjectConstantIndex)
    {
        const uint16 Dst = AllocObjectReg();
        WriteOp(EAnimOp::LoadObjectConst);
        Write(ObjectConstantIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitAdvanceClock(uint16 StateSlot, uint16 SpeedReg, uint16 ClipIndex, uint16 LoopModeReg, uint16 StartPosReg, uint16& OutFinishedReg, uint16 SyncGroup, bool bDynamicClip)
    {
        const uint16 DstClock    = AllocScalarReg();
        const uint16 DstFinished = AllocScalarReg();

        // A clock slot, so a state machine winds it back with the clock it guards and re-entry re-seeds.
        const uint16 SeededSlot  = AllocClockSlot();

        WriteOp(bDynamicClip ? EAnimOp::AdvanceClockDyn : EAnimOp::AdvanceClock);
        Write(StateSlot);
        Write(SpeedReg);
        Write(ClipIndex);
        Write(LoopModeReg);
        Write(StartPosReg);
        Write(SeededSlot);
        Write(DstClock);
        Write(DstFinished);
        Write(SyncGroup);
        OutFinishedReg = DstFinished;

        // First clip in the state wins, so a layered state reports its base clip rather than an overlay.
        if (CapturedFinishedReg == 0xFFFFu)
        {
            CapturedFinishedReg = DstFinished;
        }
        return DstClock;
    }

    uint16 FAnimationGraphCompiler::AddSyncGroup(const FName& Name)
    {
        for (int32 i = 0; i < SyncGroupNames.Num(); ++i)
        {
            if (SyncGroupNames[i] == Name)
            {
                return (uint16)i;
            }
        }
        SyncGroupNames.Add(Name);
        return (uint16)(SyncGroupNames.Num() - 1);
    }

    uint16 FAnimationGraphCompiler::AddSlot(const FName& Name)
    {
        for (int32 i = 0; i < SlotNames.Num(); ++i)
        {
            if (SlotNames[i] == Name)
            {
                return (uint16)i;
            }
        }
        SlotNames.Add(Name);
        return (uint16)(SlotNames.Num() - 1);
    }

    uint16 FAnimationGraphCompiler::EmitEvalSlot(uint16 SrcPoseReg, uint16 SlotIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::EvalSlot);
        Write(SlotIndex);
        Write(SrcPoseReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitSampleAnim(uint16 ClipIndex, uint16 TimeReg, bool bDynamicClip)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(bDynamicClip ? EAnimOp::SampleAnimDyn : EAnimOp::SampleAnim);
        Write(ClipIndex);
        Write(TimeReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitSampleBlendSpace(uint16 BlendSpaceIndex, uint16 XReg, uint16 YReg, uint16 SpeedReg, uint16 PhaseSlot,
                                                         uint16 StartPosReg, bool bDynamicBlendSpace)
    {
        const uint16 Dst = AllocPoseReg();

        // A clock slot, so a state machine winds it back with the phase it guards and re-entry re-seeds.
        const uint16 SeededSlot = AllocClockSlot();

        // Not clock slots; the seed flag above already re-snaps the smoothers, and zeroing them would pop.
        const uint16 SmoothSlot = AllocStateSlotRun(4);

        WriteOp(bDynamicBlendSpace ? EAnimOp::SampleBlendSpaceDyn : EAnimOp::SampleBlendSpace);
        Write(BlendSpaceIndex);
        Write(XReg);
        Write(YReg);
        Write(SpeedReg);
        Write(PhaseSlot);
        Write(StartPosReg);
        Write(SeededSlot);
        Write(SmoothSlot);
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

    uint16 FAnimationGraphCompiler::AddPoseSnapshot(const FName& Name)
    {
        for (int32 i = 0; i < PoseSnapshotNames.Num(); ++i)
        {
            if (PoseSnapshotNames[i] == Name)
            {
                return (uint16)i;
            }
        }
        PoseSnapshotNames.Add(Name);
        return (uint16)(PoseSnapshotNames.Num() - 1);
    }

    uint16 FAnimationGraphCompiler::EmitSavePoseSnapshot(uint16 SrcPoseReg, uint16 RequestReg, uint16 SnapshotIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::SavePoseSnapshot);
        Write(SrcPoseReg);
        Write(RequestReg);
        Write(SnapshotIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitLoadPoseSnapshot(uint16 SnapshotIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::LoadPoseSnapshot);
        Write(SnapshotIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitEaseAlpha(uint16 ValueReg, EAnimAlphaEasing Easing)
    {
        if (Easing == EAnimAlphaEasing::Linear)
        {
            return ValueReg;
        }

        const uint16 Dst = AllocScalarReg();

        WriteOp(EAnimOp::EaseAlpha);
        Write((uint8)Easing);
        Write(ValueReg);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitSmoothScalar(uint16 ValueReg, uint16 HalfLifeReg)
    {
        // The value persists, but the seed winds back with its state's clocks so re-entry snaps to the input.
        const uint16 ValueSlot  = AllocStateSlot();
        const uint16 SeededSlot = AllocClockSlot();
        const uint16 Dst        = AllocScalarReg();

        WriteOp(EAnimOp::SmoothScalar);
        Write(ValueReg);
        Write(HalfLifeReg);
        Write(ValueSlot);
        Write(SeededSlot);
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

    uint16 FAnimationGraphCompiler::EmitInertialize(uint16 SrcPoseReg, uint16 RequestReg, uint16 DurationReg, uint16 RecordIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::Inertialize);
        Write(SrcPoseReg);
        Write(RequestReg);
        Write(DurationReg);
        Write(RecordIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitDeadBlend(uint16 SrcPoseReg, uint16 RequestReg, uint16 DurationReg, uint16 HalfLifeReg, uint16 RecordIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::DeadBlend);
        Write(SrcPoseReg);
        Write(RequestReg);
        Write(DurationReg);
        Write(HalfLifeReg);
        Write(RecordIndex);
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

    uint16 FAnimationGraphCompiler::EmitMakeAdditive(uint16 SrcPoseReg, uint16 BasePoseReg, EAdditiveSpace Space)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::MakeAdditiveEx);
        Write(SrcPoseReg);
        Write(BasePoseReg);
        Write((uint8)Space);
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
        if (Name.IsNone())
        {
            return;
        }

        if (DataStruct == nullptr)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "No Parameter Struct";
            Warning.Description = FString("'") + Name.ToString() + "' cannot be read: this graph has no Parameter "
                "Struct assigned, so nothing written on the component reaches it and every parameter holds its default.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        FProperty* Property = DataStruct->GetProperty(Name);
        if (Property == nullptr)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Unknown Parameter";
            Warning.Description = FString("'") + Name.ToString() + "' is not a field on " +
                *DataStruct->GetName() + " (renamed or removed?). It will read the default value.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        if (Property->HasSetterOrGetter())
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Parameter Has Accessor";
            Warning.Description = FString("'") + Name.ToString() +
                "' is behind a getter or setter, so it cannot be read directly. It will read the default value.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        const EAnimParamValueType Type = AnimParamValueTypeFromProperty(Property);
        if (Type == EAnimParamValueType::Unresolved || Type == EAnimParamValueType::Object)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Parameter Type";
            Warning.Description = FString("'") + Name.ToString() +
                "' is not a numeric, bool, or enum field, so it can't drive a value parameter; it will read 0.";
            Warning.Node        = Node;
            AddWarning(Warning);
        }
    }

    void FAnimationGraphCompiler::ValidateObjectParameterKey(const FName& Name, EAnimObjectParamType Expected, CEdGraphNode* Node)
    {
        if (Name.IsNone())
        {
            return;
        }

        if (DataStruct == nullptr)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "No Parameter Struct";
            Warning.Description = FString("'") + Name.ToString() + "' cannot supply an asset: this graph has no "
                "Parameter Struct assigned, so the node falls back to its statically assigned asset.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        const char* ExpectedClassName = Expected == EAnimObjectParamType::BlendSpace ? "CBlendSpace" : "CAnimation";

        FProperty* Property = DataStruct->GetProperty(Name);
        if (Property == nullptr)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Unknown Parameter";
            Warning.Description = FString("'") + Name.ToString() + "' is not a field on " +
                *DataStruct->GetName() + ". It will read the default asset.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        if (AnimParamValueTypeFromProperty(Property) != EAnimParamValueType::Object)
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Parameter Type";
            Warning.Description = FString("'") + Name.ToString() +
                "' is not an object field, so it cannot supply an asset.";
            Warning.Node        = Node;
            AddWarning(Warning);
            return;
        }

        CClass* Required = FindObject<CClass>(FName(ExpectedClassName));
        CClass* Declared = static_cast<FObjectProperty*>(Property)->GetPropertyClass();
        if (Declared != nullptr && Required != nullptr && !Declared->IsChildOf(Required))
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Parameter Class";
            Warning.Description = FString("'") + Name.ToString() + "' holds a " + *Declared->GetName() +
                ", which is not a " + ExpectedClassName + ". This node would fall back to the bind pose.";
            Warning.Node        = Node;
            AddWarning(Warning);
        }
    }

    int32 FAnimationGraphCompiler::ResolveBoneIndex(const FName& BoneName) const
    {
        if (Skeleton == nullptr || BoneName.IsNone())
        {
            return Constants::kIndexNone;
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

    uint16 FAnimationGraphCompiler::EmitFABRIK(uint16 SrcPoseReg, uint16 AlphaReg,
                                               uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                                               uint16 RootIndex, uint16 TipIndex, uint16 Iterations)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::FABRIK);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(TargetXReg);
        Write(TargetYReg);
        Write(TargetZReg);
        Write(RootIndex);
        Write(TipIndex);
        Write(Iterations);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitLookAt(uint16 SrcPoseReg, uint16 AlphaReg,
                                               uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                                               uint16 BoneIndex, const FVector3& LocalForward, float MaxAngleRadians)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::LookAt);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(TargetXReg);
        Write(TargetYReg);
        Write(TargetZReg);
        Write(BoneIndex);
        Write(LocalForward);
        Write(MaxAngleRadians);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitFootIK(uint16 SrcPoseReg, uint16 AlphaReg,
                                               uint16 OffsetXReg, uint16 OffsetYReg, uint16 OffsetZReg,
                                               uint16 NormalXReg, uint16 NormalYReg, uint16 NormalZReg,
                                               uint16 AlignAlphaReg, uint16 ThighIndex, uint16 CalfIndex, uint16 FootIndex,
                                               const FVector3& FootUpAxis)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::FootIK);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(OffsetXReg);
        Write(OffsetYReg);
        Write(OffsetZReg);
        Write(NormalXReg);
        Write(NormalYReg);
        Write(NormalZReg);
        Write(AlignAlphaReg);
        Write(ThighIndex);
        Write(CalfIndex);
        Write(FootIndex);
        Write(FootUpAxis);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitTranslateBone(uint16 SrcPoseReg, uint16 AlphaReg,
                                                      uint16 XReg, uint16 YReg, uint16 ZReg, uint16 BoneIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::TranslateBone);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(XReg);
        Write(YReg);
        Write(ZReg);
        Write(BoneIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitLoadMoveAngle(const FVector3& Axis, const FVector3& Forward, float MinSpeed)
    {
        const uint16 Dst = AllocScalarReg();
        WriteOp(EAnimOp::LoadMoveAngle);
        Write(Axis);
        Write(Forward);
        Write(MinSpeed);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitAxisRotateBone(uint16 SrcPoseReg, uint16 AlphaReg, uint16 AngleReg,
                                                       const FVector3& Axis, uint16 BoneIndex)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::AxisRotateBone);
        Write(SrcPoseReg);
        Write(AlphaReg);
        Write(AngleReg);
        Write(Axis);
        Write(BoneIndex);
        Write(Dst);
        return Dst;
    }

    FAnimationGraphCompiler::FGroundTraceRegisters FAnimationGraphCompiler::EmitGroundTrace(
        uint16 FootBoneIndex, const FVector3& WorldUp,
        float TraceUpDistance, float TraceDownDistance,
        float MaxOffset, float SoleHeight,
        uint16 LayerMask, uint16 GateReg)
    {
        FGroundTraceRegisters Out;
        uint16* Registers[9] =
        {
            &Out.OffsetX, &Out.OffsetY, &Out.OffsetZ,
            &Out.NormalX, &Out.NormalY, &Out.NormalZ,
            &Out.UpX, &Out.UpY, &Out.UpZ,
        };

        for (uint16* Register : Registers)
        {
            *Register = AllocScalarReg();
        }

        WriteOp(EAnimOp::GroundTrace);
        Write(FootBoneIndex);
        Write(WorldUp);
        Write(TraceUpDistance);
        Write(TraceDownDistance);
        Write(MaxOffset);
        Write(SoleHeight);
        Write(LayerMask);
        Write(GateReg);
        for (const uint16* Register : Registers)
        {
            Write(*Register);
        }

        return Out;
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

    void FAnimationGraphCompiler::ResolveBoneMasks(const TVector<SAnimGraphBoneMaskDef>& Defs, const FSkeletonResource* InSkeleton)
    {
        Skeleton = InSkeleton;
        BoneMasks.Reset();
        BoneMaskNameToIndex.Reset();

        if (Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        if (NumBones == 0)
        {
            return;
        }

        BoneMasks.Reserve(Defs.Num());

        for (const SAnimGraphBoneMaskDef& Def : Defs)
        {
            if (Def.Name.IsNone())
            {
                continue;
            }

            FAnimGraphBoneMask Mask;
            Mask.Weights.Init(0.0f, NumBones);

            for (const SAnimGraphBoneMaskBone& Entry : Def.Bones)
            {
                const int32 BoneIdx = Skeleton->FindBoneIndex(Entry.BoneName);
                if (BoneIdx >= 0 && BoneIdx < NumBones)
                {
                    Mask.Weights[BoneIdx] = Math::Clamp(Entry.Weight, 0.0f, 1.0f);
                }
            }

            const int32 Index = BoneMasks.Num();
            BoneMasks.Add(Move(Mask));
            BoneMaskNameToIndex[Def.Name] = Index;
        }
    }

    int32 FAnimationGraphCompiler::FindBoneMaskIndex(const FName& Name) const
    {
        auto It = BoneMaskNameToIndex.Find(Name);
        return It == nullptr ? Constants::kIndexNone : (*It);
    }

    uint16 FAnimationGraphCompiler::AddBoneSubtreeMask(int32 RootBoneIndex, bool bInclusive)
    {
        FAnimGraphBoneMask Mask;
        const int32 NumBones = (Skeleton != nullptr) ? Skeleton->GetNumBones() : 0;
        Mask.Weights.Init(0.0f, NumBones);

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

        const uint16 Index = (uint16)BoneMasks.Num();
        BoneMasks.Add(Move(Mask));
        return Index;
    }

    namespace
    {
        constexpr SIZE_T kEnterSmIdxOffset     = 1;
        constexpr SIZE_T kEnterNumStatesOffset = 3;
        constexpr SIZE_T kEnterStatesEndOffset = 5;
        constexpr SIZE_T kEnterTableOffset     = 9;

        SIZE_T StateCodeEntry(uint32 EnterOp, uint16 StateIndex, bool bEnd)
        {
            return EnterOp + kEnterTableOffset + ((SIZE_T)StateIndex * 2 + (bEnd ? 1 : 0)) * sizeof(uint32);
        }
    }

    uint32 FAnimationGraphCompiler::EmitEnterStateMachine(uint16 NumStates)
    {
        const uint32 EnterOp = (uint32)Bytecode.Num();
        WriteOp(EAnimOp::EnterStateMachine);
        Write((uint16)0);
        Write(NumStates);
        Write((uint32)0);
        for (uint16 i = 0; i < NumStates; ++i)
        {
            Write((uint32)0);
            Write((uint32)0);
        }

        WriteAt(EnterOp + kEnterStatesEndOffset, (uint32)Bytecode.Num());
        EnterStateMachineOps.Add(EnterOp);
        return EnterOp;
    }

    void FAnimationGraphCompiler::BeginStateCode(uint32 EnterOp, uint16 StateIndex)
    {
        StateScopes.Add(++NextStateScope);
        if (StateIndex < ReadAt<uint16>(EnterOp + kEnterNumStatesOffset))
        {
            WriteAt(StateCodeEntry(EnterOp, StateIndex, false), (uint32)Bytecode.Num());
        }
    }

    void FAnimationGraphCompiler::EndStateCode(uint32 EnterOp, uint16 StateIndex)
    {
        if (!StateScopes.IsEmpty())
        {
            StateScopes.Pop();
        }

        if (StateIndex < ReadAt<uint16>(EnterOp + kEnterNumStatesOffset))
        {
            WriteAt(StateCodeEntry(EnterOp, StateIndex, true), (uint32)Bytecode.Num());
            WriteAt(EnterOp + kEnterStatesEndOffset, (uint32)Bytecode.Num());
        }
    }

    void FAnimationGraphCompiler::FinishStateMachineCode(uint32 EnterOp)
    {
        WriteAt(EnterOp + kEnterSmIdxOffset, (uint16)StateMachines.Num());
    }

    bool FAnimationGraphCompiler::TryGetCachedPose(const FName& Name, uint16& OutRegister)
    {
        auto It = CachedPoses.Find(Name);
        if (It == nullptr)
        {
            return false;
        }
        OutRegister = (*It);

        // A reader outside the writing state would see nothing once that state is skipped.
        auto ScopeIt = CachedPoseScopes.Find(Name);
        if (ScopeIt != nullptr)
        {
            const TVector<uint32>& WriterScopes = (*ScopeIt);
            const bool bReaderInside = WriterScopes.Num() <= StateScopes.Num() &&
                Algo::Equal(WriterScopes.begin(), WriterScopes.end(), StateScopes.begin());
            bCachedPoseLeavesItsState |= !bReaderInside;
        }
        return true;
    }

    uint16 FAnimationGraphCompiler::EmitEvalStateMachine(FAnimGraphStateMachine&& StateMachine)
    {
        const uint16 SmIndex = (uint16)StateMachines.Num();
        StateMachines.Add(Move(StateMachine));

        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::EvalStateMachine);
        Write(SmIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitGetCurve(uint16 SrcPoseReg, uint16 CurveIndex)
    {
        const uint16 Dst = AllocScalarReg();
        WriteOp(EAnimOp::GetCurve);
        Write(SrcPoseReg);
        Write(CurveIndex);
        Write(Dst);
        return Dst;
    }

    uint16 FAnimationGraphCompiler::EmitSetCurve(uint16 SrcPoseReg, uint16 CurveIndex, uint16 ValueReg)
    {
        const uint16 Dst = AllocPoseReg();
        WriteOp(EAnimOp::SetCurve);
        Write(SrcPoseReg);
        Write(CurveIndex);
        Write(ValueReg);
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
        auto It = PinRegisters.Find(Pin);
        if (It == nullptr)
        {
            return false;
        }
        OutRegister = (*It);
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

        // An end before the table makes the VM run every state in order, which keeps a shared cached pose fed.
        if (bCachedPoseLeavesItsState)
        {
            for (const uint32 EnterOp : EnterStateMachineOps)
            {
                WriteAt(EnterOp + kEnterStatesEndOffset, (uint32)0);
            }
        }

        OutGraph->Bytecode            = Bytecode;
        OutGraph->Clips               = Clips;
        OutGraph->BlendSpaces         = BlendSpaces;
        OutGraph->CurveNames          = CurveNames;
        OutGraph->ClipCurveMaps       = ClipCurveMaps;
        OutGraph->BlendSpaceCurveMaps = BlendSpaceCurveMaps;
        OutGraph->Parameters          = Parameters;
        OutGraph->ObjectParameters    = ObjectParameters;
        OutGraph->ObjectConstants     = ObjectConstants;
        OutGraph->NumObjectRegisters  = NextObjectReg;
        OutGraph->BoneMasks           = BoneMasks;
        OutGraph->StateMachines       = StateMachines;
        OutGraph->NumScalarRegisters  = NextScalarReg;
        OutGraph->NumPoseRegisters    = NextPoseReg;
        OutGraph->NumStateSlots       = NextStateSlot;
        OutGraph->NumSyncGroups       = (uint16)SyncGroupNames.Num();
        OutGraph->NumInertializerNodes = NextInertializerNode;
        OutGraph->NumDeadBlendNodes    = NextDeadBlendNode;
        OutGraph->SlotNames           = SlotNames;
        OutGraph->PoseSnapshotNames   = PoseSnapshotNames;
        OutGraph->BytecodeVersion     = kAnimBytecodeVersion;

        OutGraph->ResolveTransitionParameters();

        // Compiling changes the parameter list, so the offsets resolved at load no longer line up.
        OutGraph->LinkParameters();
    }
}
