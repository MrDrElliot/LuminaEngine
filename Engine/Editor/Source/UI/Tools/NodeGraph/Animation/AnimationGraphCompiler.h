#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
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

    // Debug breadcrumb: maps an editor State node to its runtime slot + state index so the editor can
    // highlight the VM's current state. Populated by the State Machine node.
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

    // Bytecode-emission backend: nodes call Emit*/Alloc*/Add* during CompileGraph, BuildGraph then
    // stamps the program, resource tables, and register sizing into the runtime CAnimationGraph asset.
    class EDITOR_API FAnimationGraphCompiler
    {
    public:

        FAnimationGraphCompiler() = default;

        // Registers a clip and returns its index; dedups identical clips.
        uint16 AddClip(CAnimation* Clip);

        // Registers a blend space and returns its index; dedups identical assets.
        uint16 AddBlendSpace(CBlendSpace* BlendSpace);

        // Registers a curve slot and returns its index; dedups by name. Referenced clips register their
        // authored curves automatically, so this is only needed for names a node reads or writes.
        int32 AddCurve(const FName& Name);

        // Registers an exposed parameter and returns its index; dedups by name.
        // A name collision with a different type reports an error and returns the existing index.
        int32 AddParameter(const FName& Name, EAnimGraphParamType Type, float DefaultValue);

        // Registers an object-valued input and returns its index; dedups by name.
        int32 AddObjectParameter(const FName& Name, EAnimObjectParamType Type);

        // Registers an asset a static object pin resolves to, and returns its index; dedups by pointer.
        uint16 AddObjectConstant(CObject* Value);

        uint16 AllocScalarReg() { return NextScalarReg++; }
        uint16 AllocPoseReg()   { return NextPoseReg++; }
        uint16 AllocObjectReg() { return NextObjectReg++; }
        uint16 AllocStateSlot() { return NextStateSlot++; }

        // Persistent smoothing records, one per node rather than per register.
        uint16 AllocInertializerNode() { return NextInertializerNode++; }
        uint16 AllocDeadBlendNode()    { return NextDeadBlendNode++; }

        uint16 GetInertializerNodeCount() const { return NextInertializerNode; }
        uint16 GetDeadBlendNodeCount() const { return NextDeadBlendNode; }

        // Only clocks may be wound back while inactive; zeroing a nested machine's bookkeeping thrashes it.
        uint16 AllocClockSlot()
        {
            const uint16 Slot = NextStateSlot++;
            ClockSlots.push_back(Slot);
            return Slot;
        }

        const TVector<uint16>& GetClockSlots() const { return ClockSlots; }

        // Value-producing emitters return the destination register they allocated; callers thread that
        // index into downstream emitters.
        uint16 EmitLoadConst(float Value);
        uint16 EmitLoadParam(uint16 ParameterIndex);
        uint16 EmitScalarOp(EAnimScalarOp Op, uint16 RegA, uint16 RegB);

        // Object-register producers; both return the object register they allocated.
        uint16 EmitLoadObjectParam(uint16 ObjectParameterIndex);
        uint16 EmitLoadObjectConst(uint16 ObjectConstantIndex);

        // bDynamicClip reads ClipIndex as an OBJECT REGISTER rather than a clip-table index.
        uint16 EmitAdvanceClock(uint16 StateSlot, uint16 SpeedReg, uint16 ClipIndex, uint16 LoopModeReg, uint16& OutFinishedReg,
                                uint16 SyncGroup = kAnimNoSyncGroup, bool bDynamicClip = false);

        // Registers a named sync group and returns its index; dedups by name.
        uint16 AddSyncGroup(const FName& Name);

        // Registers a montage slot name and returns its index; dedups by name.
        uint16 AddSlot(const FName& Name);

        // Layers whatever montages are playing on the slot over the incoming pose.
        uint16 EmitEvalSlot(uint16 SrcPoseReg, uint16 SlotIndex);

        uint16 EmitSampleAnim(uint16 ClipIndex, uint16 TimeReg, bool bDynamicClip = false);

        // Samples a blend space at (X, Y). The op owns its playback phase in PhaseSlot and advances it
        // against the weighted-blend duration, so the contributing clips stay stride-aligned.
        uint16 EmitSampleBlendSpace(uint16 BlendSpaceIndex, uint16 XReg, uint16 YReg, uint16 SpeedReg, uint16 PhaseSlot,
                                    bool bDynamicBlendSpace = false);
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

        // Registers a compiled state machine and emits its eval opcode. The machine's
        // StatePoseRegisters / *Slot fields must be filled by the caller. Returns the resolved-pose register.
        uint16 EmitEvalStateMachine(FAnimGraphStateMachine&& StateMachine);

        // Reads a curve slot off an incoming pose into a scalar register; writes one onto a pose,
        // returning the destination pose register (which forwards the source's pose unchanged).
        uint16 EmitGetCurve(uint16 SrcPoseReg, uint16 CurveIndex);
        uint16 EmitSetCurve(uint16 SrcPoseReg, uint16 CurveIndex, uint16 ValueReg);

        void   EmitOutput(uint16 PoseReg);
        void   EmitHalt();

        // Nodes record the register their output pin resolved to; downstream nodes look it up to thread
        // the value through. Keyed on the pin pointer, valid for one CompileGraph pass.
        void SetPinRegister(const CEdNodeGraphPin* Pin, uint16 Register);
        bool TryGetPinRegister(const CEdNodeGraphPin* Pin, uint16& OutRegister) const;

        // Captured by the editor tool after compile to drive the live debug overlay
        // (pin values, active-state highlight).
        const THashMap<const CEdNodeGraphPin*, uint16>& GetPinRegisters() const { return PinRegisters; }

        void AddDebugStateNode(CEdGraphNode* Node, uint16 CurrentStateSlot, uint16 FromStateSlot,
                               uint16 MachineIndex, int32 StateIndex)
        {
            DebugStateNodes.push_back({ Node, CurrentStateSlot, FromStateSlot, MachineIndex, StateIndex });
        }
        const TVector<FAnimGraphDebugStateNode>& GetDebugStateNodes() const { return DebugStateNodes; }

        // Machines emitted so far. One less than this is the index of the machine just emitted.
        uint16 GetStateMachineCount() const { return (uint16)StateMachines.size(); }

        // Modifies a single bone of an incoming pose. Returns the destination
        // pose register; (T, R, S) are baked into the bytecode at compile time.
        uint16 EmitBoneTransform(uint16 SrcPoseReg, uint16 AlphaReg, uint16 BoneIndex,
                                 uint16 SpaceReg, uint16 ModeReg,
                                 const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale);

        // Two-bone analytical IK. Target X/Y/Z come from scalar registers so they
        // can be driven dynamically; Pole is baked at compile time.
        uint16 EmitTwoBoneIK(uint16 SrcPoseReg, uint16 AlphaReg,
                             uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                             uint16 RootIndex, uint16 MidIndex, uint16 EndIndex,
                             const FVector3& Pole);

        // Tool calls SetSkeleton + ResolveBoneMasks once up front so nodes can look up bones / masks
        // by name in GenerateBytecode without re-fetching the asset.
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

        void ResolveBoneMasks(const TVector<FAnimGraphBoneMaskDef>& Defs, const FSkeletonResource* InSkeleton);
        int32 FindBoneMaskIndex(const FName& Name) const;

        // Ad-hoc bone mask from a bone's subtree (weight 1.0 for the root when bInclusive + descendants);
        // appended to the mask table, returns its index. Lets Layered Blend Per Bone mask by bone choice.
        uint16 AddBoneSubtreeMask(int32 RootBoneIndex, bool bInclusive);

        FORCEINLINE bool HasErrors() const { return !Errors.empty(); }
        FORCEINLINE void AddError(const EdNodeGraph::FError& Error) { Errors.push_back(Error); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetErrors() const { return Errors; }

        // Non-fatal diagnostics (e.g. a node references a renamed/removed
        // blackboard key). The graph still compiles; the tool surfaces these.
        FORCEINLINE void AddWarning(const EdNodeGraph::FError& Warning) { Warnings.push_back(Warning); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetWarnings() const { return Warnings; }

        // Writes the compiled program, clip / parameter tables, and register sizing into OutGraph.
        // Appends a Halt if the program did not end in an Output. Call once per compile.
        void BuildGraph(CAnimationGraph* OutGraph);

    private:

        // Resolves a clip's authored curves to graph curve slots, registering names as it goes.
        FAnimGraphClipCurveMap BuildClipCurveMap(CAnimation* Clip);

        template <typename T>
        void Write(const T& Value)
        {
            const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
            Bytecode.insert(Bytecode.end(), Bytes, Bytes + sizeof(T));
        }

        void WriteOp(EAnimOp Op) { Bytecode.push_back((uint8)Op); }

        TVector<uint8>                          Bytecode;
        TVector<TObjectPtr<CAnimation>>         Clips;
        TVector<TObjectPtr<CBlendSpace>>        BlendSpaces;
        TVector<FName>                          SyncGroupNames;
        TVector<FName>                          SlotNames;
        TVector<FName>                          CurveNames;
        THashMap<FName, int32>                  CurveNameToIndex;
        TVector<FAnimGraphClipCurveMap>         ClipCurveMaps;
        TVector<FAnimGraphBlendSpaceCurveMap>   BlendSpaceCurveMaps;
        TVector<FAnimGraphParameter>            Parameters;
        TVector<FAnimGraphObjectParameter>      ObjectParameters;
        TVector<TObjectPtr<CObject>>            ObjectConstants;
        TVector<FAnimGraphBoneMask>             BoneMasks;
        THashMap<FName, int32>                  BoneMaskNameToIndex;
        TVector<FAnimGraphStateMachine>         StateMachines;
        TVector<EdNodeGraph::FError>            Errors;
        TVector<EdNodeGraph::FError>            Warnings;
        THashMap<const CEdNodeGraphPin*, uint16> PinRegisters;
        TVector<FAnimGraphDebugStateNode>       DebugStateNodes;
        const FSkeletonResource*                Skeleton = nullptr;
        CStruct*                                DataStruct = nullptr;

        uint16 NextScalarReg = 0;
        uint16 NextPoseReg   = 0;
        uint16 NextObjectReg = 0;
        uint16 NextStateSlot = 0;
        uint16 NextInertializerNode = 0;
        uint16 NextDeadBlendNode = 0;

        TVector<uint16> ClockSlots;

        bool bEmittedOutput = false;
    };
}
