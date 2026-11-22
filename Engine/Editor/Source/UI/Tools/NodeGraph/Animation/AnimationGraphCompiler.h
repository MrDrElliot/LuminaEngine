#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Memory/Memcpy.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CAnimation;
    class CAnimationGraph;
    class CStruct;
    class CBlendSpace;
    class CEdGraphNode;
    struct FSkeletonResource;

    // Maps an editor State node to its runtime slot and state index so the editor can highlight the current state.
    struct FAnimGraphDebugStateNode
    {
        CEdGraphNode* Node = nullptr;
        uint16        CurrentStateSlot = 0;

        // Holds the state being blended out of, or -1 when the machine is settled.
        uint16        FromStateSlot = 0;

        // Index into CAnimationGraph::StateMachines, which is what addresses the live inertializer.
        uint16        MachineIndex = 0;

        int32         StateIndex = 0;
    };

    // Bytecode backend that nodes call during CompileGraph, with BuildGraph stamping the result into the runtime asset.
    class EDITOR_API FAnimationGraphCompiler
    {
    public:

        FAnimationGraphCompiler() = default;

        // Registers a clip and returns its index; dedups identical clips.
        uint16 AddClip(CAnimation* Clip);

        // Registers a blend space and returns its index; dedups identical assets.
        uint16 AddBlendSpace(CBlendSpace* BlendSpace);

        // Registers a curve slot and returns its index; dedups by name. Only needed for names a node reads or writes.
        int32 AddCurve(const FName& Name);

        // Registers an exposed parameter, and a name collision with a different type errors and returns the existing index.
        int32 AddParameter(const FName& Name, EAnimGraphParamType Type, float DefaultValue);

        // Registers an object-valued input and returns its index; dedups by name.
        int32 AddObjectParameter(const FName& Name, EAnimObjectParamType Type);

        // Registers an asset a static object pin resolves to, and returns its index; dedups by pointer.
        uint16 AddObjectConstant(CObject* Value);

        uint16 AllocScalarReg() { return NextScalarReg++; }
        uint16 AllocPoseReg()   { return NextPoseReg++; }
        uint16 AllocObjectReg() { return NextObjectReg++; }
        uint16 AllocStateSlot() { return NextStateSlot++; }

        // Reserves Count adjacent slots and returns the first, for an opcode that reads a block.
        uint16 AllocStateSlotRun(uint16 Count)
        {
            const uint16 First = NextStateSlot;
            NextStateSlot = (uint16)(NextStateSlot + Count);
            return First;
        }

        // Records the first clip player Finished register for a Clip Finished transition term, or 0xFFFF when no clip plays.
        void BeginStateCapture() { CapturedFinishedReg = 0xFFFFu; }
        uint16 EndStateCapture() const { return CapturedFinishedReg; }

        // Persistent smoothing records, one per node rather than per register.
        uint16 AllocInertializerNode() { return NextInertializerNode++; }
        uint16 AllocDeadBlendNode()    { return NextDeadBlendNode++; }

        uint16 GetInertializerNodeCount() const { return NextInertializerNode; }
        uint16 GetDeadBlendNodeCount() const { return NextDeadBlendNode; }

        // Only clocks may be wound back while inactive; zeroing a nested machine's bookkeeping thrashes it.
        uint16 AllocClockSlot()
        {
            const uint16 Slot = NextStateSlot++;
            ClockSlots.Add(Slot);
            return Slot;
        }

        const TVector<uint16>& GetClockSlots() const { return ClockSlots; }

        // Lets the VM run only a machine's current state, with each state's bytecode bracketed by Begin and End.
        uint32 EmitEnterStateMachine(uint16 NumStates);
        void BeginStateCode(uint32 EnterOp, uint16 StateIndex);
        void EndStateCode(uint32 EnterOp, uint16 StateIndex);

        // Call right before EmitEvalStateMachine, whose machine index it records.
        void FinishStateMachineCode(uint32 EnterOp);

        // Value-producing emitters return the destination register they allocated.
        uint16 EmitLoadConst(float Value);
        uint16 EmitLoadParam(uint16 ParameterIndex);
        uint16 EmitScalarOp(EAnimScalarOp Op, uint16 RegA, uint16 RegB);

        // Exponentially eases a scalar toward its input; a half life of 0 passes the value straight through.
        uint16 EmitSmoothScalar(uint16 ValueReg, uint16 HalfLifeReg);

        // Returns ValueReg untouched for Linear, so an unshaped alpha costs no opcode.
        uint16 EmitEaseAlpha(uint16 ValueReg, EAnimAlphaEasing Easing);

        // Object-register producers; both return the object register they allocated.
        uint16 EmitLoadObjectParam(uint16 ObjectParameterIndex);
        uint16 EmitLoadObjectConst(uint16 ObjectConstantIndex);

        // bDynamicClip reads ClipIndex as an object register rather than a clip-table index; StartPosReg seeds a wound-back clock.
        uint16 EmitAdvanceClock(uint16 StateSlot, uint16 SpeedReg, uint16 ClipIndex, uint16 LoopModeReg, uint16 StartPosReg,
                                uint16& OutFinishedReg, uint16 SyncGroup = kAnimNoSyncGroup, bool bDynamicClip = false);

        // Registers a named sync group and returns its index; dedups by name.
        uint16 AddSyncGroup(const FName& Name);

        // Registers a montage slot name and returns its index; dedups by name.
        uint16 AddSlot(const FName& Name);

        // Dedups by name so a save and a load on separate nodes address the same buffer.
        uint16 AddPoseSnapshot(const FName& Name);

        void SetCachedPose(const FName& Name, uint16 PoseRegister)
        {
            CachedPoses[Name] = PoseRegister;
            CachedPoseScopes[Name] = StateScopes;
        }

        bool TryGetCachedPose(const FName& Name, uint16& OutRegister);

        const THashMap<FName, uint16>& GetCachedPoses() const { return CachedPoses; }

        uint16 EmitSavePoseSnapshot(uint16 SrcPoseReg, uint16 RequestReg, uint16 SnapshotIndex);
        uint16 EmitLoadPoseSnapshot(uint16 SnapshotIndex);

        // Layers whatever montages are playing on the slot over the incoming pose.
        uint16 EmitEvalSlot(uint16 SrcPoseReg, uint16 SlotIndex);

        uint16 EmitSampleAnim(uint16 ClipIndex, uint16 TimeReg, bool bDynamicClip = false);

        // The op owns its playback phase in PhaseSlot and advances it against the weighted-blend duration to keep clips stride-aligned.
        uint16 EmitSampleBlendSpace(uint16 BlendSpaceIndex, uint16 XReg, uint16 YReg, uint16 SpeedReg, uint16 PhaseSlot,
                                    uint16 StartPosReg, bool bDynamicBlendSpace = false);
        uint16 EmitRefPose();
        uint16 EmitBlend(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg);
        uint16 EmitBlendMasked(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg, uint16 MaskIndex);

        // MakeAdditive deltas against BasePoseReg (kAnimNoPoseRegister = bind pose); ApplyAdditive layers it back.
        uint16 EmitMakeAdditive(uint16 SrcPoseReg, uint16 BasePoseReg = kAnimNoPoseRegister,
                                EAdditiveSpace Space = EAdditiveSpace::LocalSpace);
        uint16 EmitApplyAdditive(uint16 BasePoseReg, uint16 DeltaPoseReg, uint16 AlphaReg);

        // Smooths the seam when SrcPoseReg jumps, on the rising edge of RequestReg.
        uint16 EmitInertialize(uint16 SrcPoseReg, uint16 RequestReg, uint16 DurationReg, uint16 RecordIndex);
        uint16 EmitDeadBlend(uint16 SrcPoseReg, uint16 RequestReg, uint16 DurationReg, uint16 HalfLifeReg, uint16 RecordIndex);

        // The caller must fill the machine's StatePoseRegisters and slot fields first.
        uint16 EmitEvalStateMachine(FAnimGraphStateMachine&& StateMachine);

        // Reads a curve slot off a pose into a scalar register, or writes one onto a pose.
        uint16 EmitGetCurve(uint16 SrcPoseReg, uint16 CurveIndex);
        uint16 EmitSetCurve(uint16 SrcPoseReg, uint16 CurveIndex, uint16 ValueReg);

        void   EmitOutput(uint16 PoseReg);
        void   EmitHalt();

        // Keyed on the pin pointer, and valid for one CompileGraph pass.
        void SetPinRegister(const CEdNodeGraphPin* Pin, uint16 Register);
        bool TryGetPinRegister(const CEdNodeGraphPin* Pin, uint16& OutRegister) const;

        // Captured after compile to drive the live debug overlay.
        const THashMap<const CEdNodeGraphPin*, uint16>& GetPinRegisters() const { return PinRegisters; }

        void AddDebugStateNode(CEdGraphNode* Node, uint16 CurrentStateSlot, uint16 FromStateSlot,
                               uint16 MachineIndex, int32 StateIndex)
        {
            DebugStateNodes.Emplace(Node, CurrentStateSlot, FromStateSlot, MachineIndex, StateIndex);
        }
        const TVector<FAnimGraphDebugStateNode>& GetDebugStateNodes() const { return DebugStateNodes; }

        // The register holding the time a clip node samples at, which the debug view draws as its playhead.
        void SetDebugPlayhead(const CEdGraphNode* Node, uint16 TimeRegister) { DebugPlayheads[Node] = TimeRegister; }
        const THashMap<const CEdGraphNode*, uint16>& GetDebugPlayheads() const { return DebugPlayheads; }

        // Machines emitted so far. One less than this is the index of the machine just emitted.
        uint16 GetStateMachineCount() const { return (uint16)StateMachines.Num(); }

        // Translation, rotation and scale are baked into the bytecode at compile time.
        uint16 EmitBoneTransform(uint16 SrcPoseReg, uint16 AlphaReg, uint16 BoneIndex,
                                 uint16 SpaceReg, uint16 ModeReg,
                                 const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale);

        // Target comes from scalar registers so it can be driven dynamically, while Pole is baked at compile time.
        uint16 EmitFABRIK(uint16 SrcPoseReg, uint16 AlphaReg,
                          uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                          uint16 RootIndex, uint16 TipIndex, uint16 Iterations);

        uint16 EmitLookAt(uint16 SrcPoseReg, uint16 AlphaReg,
                          uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                          uint16 BoneIndex, const FVector3& LocalForward, float MaxAngleRadians);

        uint16 EmitFootIK(uint16 SrcPoseReg, uint16 AlphaReg,
                          uint16 OffsetXReg, uint16 OffsetYReg, uint16 OffsetZReg,
                          uint16 NormalXReg, uint16 NormalYReg, uint16 NormalZReg,
                          uint16 AlignAlphaReg, uint16 ThighIndex, uint16 CalfIndex, uint16 FootIndex,
                          const FVector3& FootUpAxis);

        uint16 EmitTranslateBone(uint16 SrcPoseReg, uint16 AlphaReg,
                                 uint16 XReg, uint16 YReg, uint16 ZReg, uint16 BoneIndex);

        // One trace's component-space results, Up being the trace axis rotated into that same space.
        struct FGroundTraceRegisters
        {
            uint16 OffsetX = 0, OffsetY = 0, OffsetZ = 0;
            uint16 NormalX = 0, NormalY = 0, NormalZ = 0;
            uint16 UpX = 0, UpY = 0, UpZ = 0;
        };

        // Signed radians from Forward to the entity's own velocity about Axis; below MinSpeed it reads zero.
        uint16 EmitLoadMoveAngle(const FVector3& Axis, const FVector3& Forward, float MinSpeed);

        // Turns one bone about a component-space axis by however many radians AngleReg holds.
        uint16 EmitAxisRotateBone(uint16 SrcPoseReg, uint16 AlphaReg, uint16 AngleReg,
                                  const FVector3& Axis, uint16 BoneIndex);

        // GateReg at or below zero skips the query entirely, so a faded-out node costs no raycast.
        FGroundTraceRegisters EmitGroundTrace(uint16 FootBoneIndex, const FVector3& WorldUp,
                                              float TraceUpDistance, float TraceDownDistance,
                                              float MaxOffset, float SoleHeight,
                                              uint16 LayerMask, uint16 GateReg);

        uint16 EmitTwoBoneIK(uint16 SrcPoseReg, uint16 AlphaReg,
                             uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                             uint16 RootIndex, uint16 MidIndex, uint16 EndIndex,
                             const FVector3& Pole);

        // Set once up front so nodes can look up bones and masks by name in GenerateBytecode.
        void SetSkeleton(const FSkeletonResource* InSkeleton) { Skeleton = InSkeleton; }
        const FSkeletonResource* GetSkeleton() const { return Skeleton; }

        // Tool sets the parameter struct before compiling so nodes can verify the names they reference.
        void SetDataStruct(CStruct* InStruct) { DataStruct = InStruct; }
        CStruct* GetDataStruct() const { return DataStruct; }

        // Warns when Name is not a readable scalar field on the parameter struct.
        void ValidateParameterKey(const FName& Name, CEdGraphNode* Node);

        // Warns when Name is not an object field, or its class cannot supply the expected asset.
        void ValidateObjectParameterKey(const FName& Name, EAnimObjectParamType Expected, CEdGraphNode* Node);

        int32 ResolveBoneIndex(const FName& BoneName) const;

        void ResolveBoneMasks(const TVector<SAnimGraphBoneMaskDef>& Defs, const FSkeletonResource* InSkeleton);
        int32 FindBoneMaskIndex(const FName& Name) const;

        // Ad-hoc mask from a bone's subtree, which lets Layered Blend Per Bone mask by bone choice.
        uint16 AddBoneSubtreeMask(int32 RootBoneIndex, bool bInclusive);

        FORCEINLINE bool HasErrors() const { return !Errors.IsEmpty(); }
        FORCEINLINE void AddError(const EdNodeGraph::FError& Error) { Errors.Add(Error); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetErrors() const { return Errors; }

        // Non-fatal diagnostics, such as a renamed or removed blackboard key, which the tool surfaces.
        FORCEINLINE void AddWarning(const EdNodeGraph::FError& Warning) { Warnings.Add(Warning); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetWarnings() const { return Warnings; }

        // Appends a Halt if the program did not end in an Output. Call once per compile.
        void BuildGraph(CAnimationGraph* OutGraph);

    private:

        // Resolves a clip's authored curves to graph curve slots, registering names as it goes.
        FAnimGraphClipCurveMap BuildClipCurveMap(CAnimation* Clip);

        template <typename T>
        void Write(const T& Value)
        {
            const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
            Bytecode.Append(Bytes, Bytes + sizeof(T));
        }

        void WriteOp(EAnimOp Op) { Bytecode.Add((uint8)Op); }

        template <typename T>
        void WriteAt(SIZE_T Offset, const T& Value)
        {
            Memory::Memcpy(Bytecode.GetData() + Offset, &Value, sizeof(T));
        }

        template <typename T>
        T ReadAt(SIZE_T Offset) const
        {
            T Value;
            Memory::Memcpy(&Value, Bytecode.GetData() + Offset, sizeof(T));
            return Value;
        }

        TVector<uint8>                          Bytecode;
        TVector<TStrongObjectPtr<CAnimation>>   Clips;
        TVector<TStrongObjectPtr<CBlendSpace>>  BlendSpaces;
        TVector<FName>                          SyncGroupNames;
        TVector<FName>                          SlotNames;
        TVector<FName>                          PoseSnapshotNames;
        TVector<FName>                          CurveNames;
        THashMap<FName, int32>                  CurveNameToIndex;
        TVector<FAnimGraphClipCurveMap>         ClipCurveMaps;
        TVector<FAnimGraphBlendSpaceCurveMap>   BlendSpaceCurveMaps;
        TVector<FAnimGraphParameter>            Parameters;
        TVector<SAnimGraphObjectParameter>      ObjectParameters;
        TVector<TStrongObjectPtr<CObject>>      ObjectConstants;
        TVector<FAnimGraphBoneMask>             BoneMasks;
        THashMap<FName, int32>                  BoneMaskNameToIndex;
        THashMap<FName, uint16>                 CachedPoses;
        THashMap<FName, TVector<uint32>>        CachedPoseScopes;
        TVector<FAnimGraphStateMachine>         StateMachines;
        TVector<EdNodeGraph::FError>            Errors;
        TVector<EdNodeGraph::FError>            Warnings;
        THashMap<const CEdNodeGraphPin*, uint16> PinRegisters;
        TVector<FAnimGraphDebugStateNode>       DebugStateNodes;
        THashMap<const CEdGraphNode*, uint16>   DebugPlayheads;
        const FSkeletonResource*                Skeleton = nullptr;
        CStruct*                                DataStruct = nullptr;

        uint16 NextScalarReg = 0;
        uint16 NextPoseReg   = 0;
        uint16 NextObjectReg = 0;
        uint16 NextStateSlot = 0;
        uint16 CapturedFinishedReg = 0xFFFFu;
        uint16 NextInertializerNode = 0;
        uint16 NextDeadBlendNode = 0;

        TVector<uint16> ClockSlots;

        // Innermost last, so a cached pose read outside the state that wrote it can be caught.
        TVector<uint32> StateScopes;
        uint32 NextStateScope = 0;
        TVector<uint32> EnterStateMachineOps;
        bool bCachedPoseLeavesItsState = false;

        bool bEmittedOutput = false;
    };
}
